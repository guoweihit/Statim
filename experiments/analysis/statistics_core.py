# SPDX-License-Identifier: GPL-3.0-or-later
"""Statistics over independent simulation seeds and recorded timing batches."""

from collections import defaultdict
import json
import math

import numpy as np
from scipy import stats


SEED_FIELDS = {"seed", "run", "rng_seed", "rng_run"}
REPLICATE_FIELDS = SEED_FIELDS | {"invocation", "repetition", "replicate"}


def finite_number(value):
    """Convert an observed numeric value, with None representing missing data."""
    if value is None or value == "":
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def estimate(values, confidence=0.95):
    """Student-t interval for a mean; every input contributes to data accounting.

    The sample unit is an independent seed (or a separately identified timing
    repetition). Missing and invalid values retain their counts in the result.
    A singleton has an observed mean and an unavailable confidence interval.
    """
    if not 0 < confidence < 1:
        raise ValueError("confidence must lie strictly between zero and one")
    values = list(values)
    numbers = [finite_number(value) for value in values]
    array = np.asarray([value for value in numbers if value is not None], dtype=float)
    missing = sum(value is None or value == "" for value in values)
    n = len(array)
    result = dict(n=n, total=len(values), missing=missing,
                  invalid=len(values) - missing - n, mean=None, sd=None,
                  median=None, minimum=None, maximum=None, confidence=confidence,
                  ci_low=None, ci_high=None, ci_half_width=None,
                  ci_status="insufficient_samples", df=max(n - 1, 0))
    if n:
        result.update(mean=float(np.mean(array)), median=float(np.median(array)),
                      minimum=float(np.min(array)), maximum=float(np.max(array)))
    if n >= 2:
        sd = float(np.std(array, ddof=1))
        half = float(stats.t.ppf((1 + confidence) / 2, n - 1) * sd / np.sqrt(n))
        result.update(sd=sd, ci_low=result["mean"] - half,
                      ci_high=result["mean"] + half, ci_half_width=half,
                      ci_status="estimated" if sd else "zero_sample_variance")
    return result


def regression(xs, ys, confidence=0.95):
    """Ordinary least squares over available paired observations.

    Slope intervals use the residual sum of squares. R-squared is available
    for varying responses; a constant response has an undefined R-squared.
    """
    if not 0 < confidence < 1:
        raise ValueError("confidence must lie strictly between zero and one")
    xs, ys = list(xs), list(ys)
    if len(xs) != len(ys):
        raise ValueError("regression inputs must have equal lengths")
    pairs = [(finite_number(x), finite_number(y)) for x, y in zip(xs, ys)]
    valid = [(x, y) for x, y in pairs if x is not None and y is not None]
    result = dict(n=len(valid), total=len(xs), missing_pairs=len(xs) - len(valid),
                  predictor_values=sorted({x for x, _ in valid}),
                  slope=None, intercept=None, r_squared=None, slope_ci_low=None,
                  slope_ci_high=None, ci_status="insufficient_samples",
                  r_squared_status="insufficient_samples")
    if len(valid) < 2:
        return result
    x, y = np.asarray(valid).T
    if np.unique(x).size < 2:
        result["ci_status"] = "constant_predictor"
        result["r_squared_status"] = "constant_predictor"
        return result
    fitted = stats.linregress(x, y)
    constant_response = np.ptp(y) == 0
    r_squared = None if constant_response else finite_number(fitted.rvalue ** 2)
    result.update(slope=float(fitted.slope), intercept=float(fitted.intercept),
                  r_squared=r_squared,
                  r_squared_status="constant_response" if constant_response else
                      ("defined" if r_squared is not None else "unavailable"))
    if len(valid) >= 3:
        if constant_response:
            standard_error = 0.0
        else:
            centered_x, centered_y = x - np.mean(x), y - np.mean(y)
            residuals = centered_y - fitted.slope * centered_x
            residual_sum_squares = float(np.dot(residuals, residuals))
            predictor_sum_squares = float(np.dot(centered_x, centered_x))
            standard_error = finite_number(np.sqrt(
                residual_sum_squares / (len(valid) - 2) / predictor_sum_squares))
        if standard_error is not None:
            half = float(stats.t.ppf((1 + confidence) / 2, len(valid) - 2) * standard_error)
            result.update(slope_ci_low=float(fitted.slope - half),
                          slope_ci_high=float(fitted.slope + half),
                          ci_status="estimated" if standard_error else "zero_residual_variance")
        else:
            result["ci_status"] = "unavailable_standard_error"
    return result


def key(parameters):
    def normalized(value):
        if isinstance(value, dict):
            return {name: normalized(item) for name, item in value.items()}
        if isinstance(value, (list, tuple)):
            return [normalized(item) for item in value]
        if isinstance(value, float) and math.isfinite(value) and value.is_integer():
            return int(value)
        return value
    return json.dumps(normalized(parameters), sort_keys=True, separators=(",", ":"))


def sample_parameters(record, excluded=()):
    return {name: value for name, value in record["parameters"].items()
            if name not in set(excluded) | REPLICATE_FIELDS}


def seed_key(record):
    return key({name: record["parameters"][name] for name in sorted(SEED_FIELDS)
                if name in record["parameters"]})


def metric_value(record, name):
    """Expose an invalid-value sentinel only to the in-memory estimator."""
    if record.get("metric_states", {}).get(name) == "invalid":
        return float("nan")
    return record["metrics"].get(name)


def summaries(records, confidence=0.95):
    """Group by every effective parameter and retain the independent sample count."""
    groups = defaultdict(list)
    for record in records:
        if record.get("repetition_of"):
            continue
        parameters = sample_parameters(record)
        groups[(record["target"], key(parameters))].append(record)
    result = []
    for (target, parameters), members in sorted(groups.items()):
        metrics = sorted({metric for record in members for metric in record["metrics"]})
        for metric in metrics:
            result.append(dict(target=target, parameters=json.loads(parameters), metric=metric,
                experiments=sorted({item for record in members for item in record.get("experiments", [])}),
                analysis_views=sorted({item for record in members for item in record.get("analysis_views", [])}),
                **estimate([metric_value(record, metric) for record in members], confidence)))
    return result


def paired_effects(records, confidence=0.95):
    """Enabled-minus-disabled feature contrasts matched by all parameters and seed."""
    output = []
    for feature in ("temporary_forwarding", "interest_reforwarding", "consumer_retransmissions"):
        groups = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
        for record in records:
            if record.get("repetition_of") or feature not in record["parameters"]:
                continue
            if record["parameters"][feature] not in (True, False, 0, 1):
                continue
            if seed_key(record) == "{}":
                continue
            group = (record["target"], key(sample_parameters(record, [feature])))
            groups[group][seed_key(record)][bool(record["parameters"][feature])].append(record)
        for (target, parameters), seeds in sorted(groups.items()):
            observed_levels = {level for sides in seeds.values() for level in sides}
            if observed_levels != {True, False}:
                continue
            metrics = sorted({metric for sides in seeds.values() for members in sides.values()
                              for record in members for metric in record["metrics"]})
            for metric in metrics:
                differences, unpaired, invalid, ambiguous = [], 0, 0, 0
                for sides in seeds.values():
                    if True not in sides or False not in sides:
                        unpaired += 1
                    elif len(sides[True]) != 1 or len(sides[False]) != 1:
                        ambiguous += 1
                    else:
                        a = finite_number(sides[True][0]["metrics"].get(metric))
                        b = finite_number(sides[False][0]["metrics"].get(metric))
                        if a is None or b is None:
                            invalid += 1
                        else:
                            differences.append(a - b)
                output.append(dict(target=target, parameters=json.loads(parameters),
                    feature=feature, metric=metric, contrast="enabled_minus_disabled",
                    enabled_seeds=sum(True in sides for sides in seeds.values()),
                    disabled_seeds=sum(False in sides for sides in seeds.values()),
                    unmatched_seeds=unpaired, missing_metric_pairs=invalid,
                    ambiguous_seeds=ambiguous, **estimate(differences, confidence)))
    return output


def delay_regressions(records, confidence=0.95):
    """Fit each seed's delay sweep, then estimate the mean slope across seeds.

    Reusing a seed across delay settings gives paired observations. The final
    confidence interval uses the independently generated seed-level slopes.
    """
    groups = defaultdict(lambda: defaultdict(list))
    for record in records:
        if record.get("repetition_of") or "controller_delay_s" not in record["parameters"]:
            continue
        if seed_key(record) == "{}":
            continue
        group = (record["target"], key(sample_parameters(record, ["controller_delay_s"])))
        groups[group][seed_key(record)].append(record)
    output = []
    for (target, parameters), seeds in sorted(groups.items()):
        metrics = sorted({name for runs in seeds.values() for run in runs for name in run["metrics"]})
        for metric in metrics:
            slopes, intercepts, details = [], [], []
            for seed, runs in sorted(seeds.items()):
                xs = [row["parameters"]["controller_delay_s"] for row in runs]
                if len(set(xs)) != len(xs):
                    details.append(dict(seed=json.loads(seed), ci_status="duplicate_delay_cells"))
                    continue
                fitted = regression(xs, [row["metrics"].get(metric) for row in runs], confidence)
                details.append(dict(seed=json.loads(seed), **fitted))
                if fitted["slope"] is not None:
                    slopes.append(fitted["slope"])
                    intercepts.append(fitted["intercept"])
            if slopes:
                output.append(dict(target=target, parameters=json.loads(parameters), metric=metric,
                    predictor="controller_delay_s", seed_fits=details,
                    slope=estimate(slopes, confidence), intercept=estimate(intercepts, confidence)))
    return output
