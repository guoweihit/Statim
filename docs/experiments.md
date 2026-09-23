# Experiment guide

[中文](experiments.zh-CN.md) · [README](../README.md) · [Design and implementation](model.md)

The experiments examine routing-update timing during producer handover and the temporary state held while controller installation is pending.
This guide covers the execution environment, experiment settings, measurements, and statistical analysis.

## Build and first run

The supplied ns-3 tree contains the fixed dependencies listed in [BASELINE](../BASELINE.md).
Build on Linux x86-64 using Ubuntu 18.04, GCC 7.5, and Python 2.7 for Waf.
The experiment runner uses Python 3.6 or later and the standard library.

The project environment can be built and started from a Linux or WSL shell at the repository root:

```sh
docker build --platform linux/amd64 -f Dockerfile.environment -t statim-environment .
mkdir -p ../statim-results
docker run --rm -it --platform linux/amd64 \
  --mount type=bind,src="$(pwd)",dst=/code \
  --mount type=bind,src="$(cd ../statim-results && pwd)",dst=/results \
  --workdir /code statim-environment bash
```

Inside this environment, build the model and run a quick check:

```sh
python2.7 waf configure -d optimized --enable-modules=ndnSIM --disable-examples --disable-tests --disable-python
python2.7 waf -j8
python3 experiments/run.py --profile quick --output /results/quick --jobs 8
```

The build produces nine program targets under `build/src/ndnSIM/statim/`.
The runner locates the selected targets and sets their shared-library search path.
Choose a fresh result directory outside the source tree for each run.
Results written to `/results` remain in the host's `statim-results` directory.
`execution-summary.json` gives the completion count and any failed jobs; each process record links the corresponding command and logs.

For the full configured seed set and workloads, run:

```sh
python3 experiments/run.py --profile full --output /results/full --jobs 8
```

## Experiment questions

| Group | Purpose and comparison |
| --- | --- |
| Handover quality | Varies controller installation delay and temporary forwarding, with a KITE-NFD reference. Measures handover latency and unsatisfied requests with consumer retransmission enabled and disabled. |
| Processing delay | Adds the same per-pass delay to the two Statim forwarding configurations to measure how local processing shifts the handover and loss curves. |
| Interest reforwarding | Resends remembered Interests on the updated next hop while temporary forwarding is disabled. Measures this action's contribution to recovery. |
| Completion-notice loss | Drops completion notices after route installation and observes retries, temporary-state cleanup, and final routes. |
| Path sensitivity | Varies the number of router-to-router links and consumer join position, comparing temporary forwarding enabled and disabled at the selected controller delays. |
| Occupancy | Varies distinct-prefix arrival rate, installation delay, and initial burst to measure active temporary entries and their residence times. |
| Functional checks | Exercises control-event ordering and losses, sequential lookup, state-container behavior, and packet-pipeline behavior. |
| Optional software profiling | Measures selected lookup and update operations on the execution host. |

The model's packet and update semantics are described in [Design and implementation](model.md).
Final-route checks, same-prefix replacement, and signaling counts are additional analyses of the mobility runs.
The run manifest associates each unique simulation with all experiments and analysis views that use it.

## Configure an experiment

[config.ini](../experiments/config.ini) contains the editable configuration, with English and Chinese comments.
Values use JSON literals: numbers, `true`/`false`, quoted strings, and arrays.
Whole-line `#` comments explain units, defaults, valid choices, and parameter relationships.

`[defaults]` defines the complete base experiment template.
Each `[experiment.NAME]` selects an experiment and overrides the fields that differ from that template.
Set `enabled = false` in an experiment section to retain its settings while excluding its runs; `analysis_views` can add descriptive labels to its results.
For example, `[experiment.handover_quality]` inherits the complete comparison, while the processing-delay group selects its smaller delay sweep and additional processing time.
Experiment comments explain each choice's scientific purpose.
The template supplies inherited values; only the named experiment sections create runnable groups.
Fields for other scenarios remain in the inherited template, while each scenario passes its applicable fields to its program.
An explicit override must belong to the selected scenario and system; the loader validates enabled and disabled sections alike.

The main comparison uses these independent choices:

| Setting | Meaning |
| --- | --- |
| `systems` | `statim` selects the Statim forwarding model; `nfd-kite` selects the fixed KITE-enabled NFD stack. |
| `temporary_forwarding` | In Statim, `true` uses a matching local temporary route; `false` uses controller-installed forwarding routes. Temporary updates and control exchanges still run in both settings. |
| `interest_reforwarding` | Controls forwarding of remembered Interests after a route update. |
| `consumer_retransmissions` | Controls repeated consumer transmissions for outstanding sequences. |
| `controller_delay_s` | Decision-to-installation delay, in simulated seconds, for the Statim controller service. |
| `seed_first`, `seed_last` | Inclusive independent seed interval. |

An array expands a dimension of the experiment.
The NFD-KITE reference uses its native local routing update path.
Its runs vary the shared mobility, traffic, consumer-retransmission, and Interest-reforwarding parameters.
Controller delay, temporary forwarding, processing delay, and the retry policy are Statim dimensions.
When a group selects both systems, those dimensions expand only the Statim runs; the NFD-KITE observation is shared across them.
The manifest records only the effective parameters of each run.
Configurations that produce the same target, effective arguments, and seed share a single execution, with all uses retained in the manifest.

### Scenarios and parameter scope

| `scenario` | Program and topology | Available systems |
| --- | --- | --- |
| `handover` | `statim-grid` or `kite-grid`: six routers; rendezvous at R1, consumer at R4, producer moves from R3 to R6 | `statim`, `nfd-kite` |
| `path` | `statim-path`: chain R0 to RH, rendezvous at R0, old producer access on a branch at R0, new access at RH | `statim` |
| `occupancy` | `temporary-fib-occupancy`: unique-prefix arrivals and controller completions through the Statim state-update path | `statim` |

Both handover programs calculate routes with a face cost of one per hop.
The shared [grid topology](../src/ndnSIM/statim/scenarios/common/grid-topology.txt) records these unit costs and fixes the router and link order used by both programs.

The parameter groups below cover all fields in `[defaults]`.
Seconds refer to simulated time; `stateful_delay_us` uses microseconds.

| Fields | Scope and interpretation |
| --- | --- |
| `scenario`, `systems`, `seed_first`, `seed_last` | All scenarios. The seed interval is inclusive, from 1 to 4294967295. Mobility programs set ns-3 `RngRun`; occupancy sets `RngSeed` and uses run 1. |
| `consumer_retransmissions`, `interest_reforwarding` | Both mobility scenarios and both available systems. These are independent Boolean choices. |
| `consumer_cbr_frequency`, `payload_bytes` | Both mobility scenarios and systems. Positive requests/s and nonnegative Data payload bytes, respectively. |
| `handover_base_s`, `handover_jitter_s`, `observation_window_s`, `stop_s` | Both mobility scenarios and systems. Handover occurs at `base + U[0, jitter]`; the observation window starts at that handover. |
| `refresh_interval_s`, `trace_lifetime_s`, `interest_lifetime_s`, `initial_rtt_estimate_s` | Both mobility scenarios and systems. Trace cadence, learned-route lifetime, Interest packet lifetime, and initial consumer RTT estimate, respectively. |
| `temporary_forwarding`, `stateful_delay_us` | Statim mobility only. Temporary-route forwarding and added module-processing delay. |
| `controller_delay_s` | Statim mobility: nonnegative decision-to-installation delay Tc. Occupancy: strictly positive completion delay Tc for each arrival. |
| `interest_reforwarding_limit`, `generation_guard` | Statim `handover` only. Maximum remembered Interests sent per accepted update, and persistent generation checks. |
| `completion_notice_loss`, `controller_retry_limit` | Statim `handover` only. Completion-notice loss probability in [0, 1] and maximum additional attempts after the first request. |
| `controller_retry_timeout_s`, `retry_timeout_margin_s` | Statim `handover` only. A nonnegative retry timeout or `"auto"`; auto uses `2 * controller_delay_s + retry_timeout_margin_s` for positive notice loss and zero otherwise. The margin is used only in the auto calculation. Zero timeout leaves completion handling event-driven. |
| `trace_path_hops`, `consumer_join` | `path` only. H is an integer of at least 2 router links. `"rendezvous"` joins the consumer at R0; `"middle"` uses router index `floor(H/2)`. |
| `arrival_rate_per_s`, `arrival_count`, `initial_burst` | `occupancy` only. Positive exponential-arrival rate, positive total prefix count, and simultaneous arrivals at time zero. The burst is included in the total count and lies between zero and `arrival_count`. |

The path program fixes `interest_reforwarding_limit=1000`, `generation_guard=false`, `controller_retry_limit=3`, `controller_retry_timeout_s=0`, and `completion_notice_loss=0`.
Its manifest records those values; select Statim `handover` to vary this control policy.
Occupancy ends after the final arrival completes, so its horizon follows the arrival workload and Tc.
Network-traffic and mobility-window settings apply to the two mobility scenarios.

Array-valued dimensions are `systems`, `consumer_retransmissions`, `interest_reforwarding`, `temporary_forwarding`, `controller_delay_s`, `stateful_delay_us`, `completion_notice_loss`, `trace_path_hops`, `consumer_join`, `arrival_rate_per_s`, and `initial_burst`.
Each accepts one value or a nonempty array of distinct values; the remaining fields take one value.
Numeric values must be finite.
Integer counts and limits fit an unsigned 32-bit value; `payload_bytes`, `interest_reforwarding_limit`, and `controller_retry_limit` use signed integers and are at most 2147483647.

Interest lifetime and the initial RTT estimate are separate settings.
Their configuration keys are `interest_lifetime_s` and `initial_rtt_estimate_s`.
Interest lifetime governs the lifetime carried by a request; the RTT estimate initializes the consumer's retransmission-timeout estimator.
Both default to 2 seconds and can be changed independently.
Interest and Trace lifetimes are at least 0.001 seconds and use whole milliseconds, for example 0.001, 0.250, or 2.
The initial RTT estimate accepts finite positive values, including fractional milliseconds at simulator time resolution.
The producer's Trace refresh interval and tracing lifetime separately govern signaling cadence and learned-route lifetime.
Their relation to controller delay determines whether an update remains current and valid when its completion arrives.
The main comparison sets a 5 s route lifetime and a 2 s refresh interval for both implementations.
For its maximum 2 s controller delay, this satisfies the steady-path overlap condition `lifetime > refresh interval + controller delay`.
Statim fixes each route's absolute deadline at local acceptance; the controller installs only the remaining lease, and rejects updates at or after that deadline.
The `route_expiry_boundary` study uses a 2 s route lifetime and 2 s refresh interval, controller delays of 0, 0.5 and 2 s, and both dual-FIB choices with consumer retransmission and Interest reforwarding disabled.
It retains expired-installation and missing-handover observations as boundary results.

Consumers send from 1 second until `stop_s - 2`; the producer stops at `stop_s - 1`.
The consumer finalizes its request-satisfaction counters when it stops, and the simulation continues until `stop_s` to collect the final controller and forwarding state.
Use `handover_base_s >= 1`, nonnegative jitter, and `handover_base_s + handover_jitter_s + observation_window_s < stop_s - 2`.
The observation window and stop time are positive.
Path isolation also requires both Trace lifetime and refresh interval to exceed `stop_s`, so the measured handover contains one tracing round.
The `[checks]` section selects the standalone functional programs, each executed once.

`full` uses every selected seed and the configured workload sizes.
`quick` uses the first selected seed per cell and smaller occupancy and software-profiling workloads.
Its occupancy workload preserves the configured initial burst and, when present in the full workload, at least one following arrival.
Read `process_count` and `counts` in the run manifest for the actual expanded size.

To use a separate configuration:

```sh
cp experiments/config.ini /path/to/my-experiments.ini
python3 experiments/run.py --config /path/to/my-experiments.ini --profile full --output /results/custom
```

For example, edit the existing handover-quality section in that copy to use ten seeds and three Tc values:

```ini
[experiment.handover_quality]
seed_first = 101
seed_last = 110
controller_delay_s = [0, 0.1, 0.5]
```

This overrides only that group; the other enabled groups retain their settings.
Edit `[defaults]` to change values inherited by all groups, or set `enabled = false` in individual experiment sections to select a smaller study.
Use `full` to execute all ten seeds; `quick` executes seed 101 for this group.

### Runner options

| Option | Default | Purpose |
| --- | --- | --- |
| `--config PATH` | `experiments/config.ini` | Annotated experiment settings |
| `--profile {full,quick}` | `full` | Seed and workload profile |
| `--output PATH` | Required | Fresh output directory outside the source tree |
| `--jobs N`, `-j N` | `8` | Concurrent simulation processes |
| `--timeout SECONDS` | `300` | Wall-clock limit for each process |
| `--microbenchmark` | Configuration value | Enable optional host software profiling |
| `--no-microbenchmark` | Configuration value | Disable optional host software profiling |
| `--cpu N` | First allowed logical CPU | CPU for serial software-profiling invocations |

The supplied configuration disables software profiling.
Keep `[microbenchmark] enabled = false`, or pass `--no-microbenchmark` to force it off for a run:

```sh
python3 experiments/run.py --profile full --no-microbenchmark --output /results/full
```

Enable it explicitly to collect host timing:

```sh
python3 experiments/run.py --profile full --microbenchmark --output /results/full-with-profiling
```

The `[microbenchmark]` section selects operation counts, batch size, repetitions within an invocation, and the number of independent invocations.
Timing processes run serially after simulations finish.
Record host load, frequency conditions, image identity, and CPU selection with timing results.

Standalone checks are also available through Waf:

```sh
python2.7 waf --run state-table-tests
python2.7 waf --run statim-packet-pipeline-tests
python2.7 waf --run "statim-grid --PrintHelp"
```

The `checks.programs` choices are `state-table-tests` (state-container cases), `statim-packet-pipeline-tests` (packet encoding, pipeline, switch, cache, and consumer counters), `lpm-consistency` (temporary-first and combined-table lookup selections), and `statim-controller-faults` (control-event cases).
The control cases cover normal completion, an identical duplicate update, conflicting payloads at the same generation, delayed old requests/installations/completions, and one-time or persistent loss on each of the three control legs.
Eight further cases check shared expiry, expiry at installation, expiry before control delivery, retries with a fixed deadline, completion after expiry, a conflicting deadline at the same generation, renewed expiry for a new generation, and rejected installation without a success notice.

## Analyze a run

Analysis uses Python 3.10 or later with NumPy and SciPy, in a separate host environment.
From the repository root:

```sh
python3 -m pip install -r requirements-analysis.txt
python3 experiments/analyze.py \
  --run-root ../statim-results/full \
  --output ../statim-results/analysis/report.json \
  --verify
```

The analyzer uses effective parameters recorded in the run manifest for grouping, so full, quick, and custom configurations use the same entry point.
It writes the main JSON report to `--output` and tables to a sibling directory obtained by removing that path's suffix, for example `report.json` and `report/`.
Use `--tables PATH` to choose an empty table directory explicitly.
`--confidence` selects the two-sided confidence level, defaulting to 0.95.
`--verify` also checks recorded file hashes and individual occupancy residence records.
`--data-root PATH` adds a comparison with a historical run directory or previously generated `records.json`.

Read `issues.json` for execution and data-integrity errors.
`mechanism-observations.json` separately reports model observations, including cases constructed to demonstrate different lookup selections.
Optional groups contribute results when included in the run.

### Statistical unit and comparisons

The statistical unit for simulation summaries is one independent seed at a fixed effective parameter combination.
The analyzer groups by every non-replicate parameter, records the actual sample count, and computes a Student-t confidence interval using SciPy's quantile function with `n - 1` degrees of freedom.
The summary reports `total` successfully loaded records after duplicate-seed handling, `n` finite measurements, `missing` absent values, and `invalid` nonfinite or malformed values.
For `n=0`, the mean, standard deviation, and interval are unavailable.
For `n=1`, the mean is the observed value; the standard deviation and interval are unavailable.
Both cases use `ci_status=insufficient_samples`.
Two or more equal samples produce a zero-width interval marked `zero_sample_variance`.
Execution failures appear in `issues.json` and the main report; `total` counts the successfully loaded records, so check those errors alongside the metric counts.
A metric absent from every record in a group produces no summary row.

Feature comparisons pair enabled and disabled observations with the same seed and all other effective parameters equal.
The reported contrast is enabled minus disabled.
`missing_metric_pairs`, `unmatched_seeds`, and `ambiguous_seeds` record incomplete or ambiguous comparisons separately.
Controller-delay analysis first fits a line within each seed's delay sweep, then summarizes slopes and intercepts across seeds.
This preserves the pairing of observations that reuse a seed across delays.
Its predictor uses seconds, so a handover-latency slope of 1000 ms/s corresponds to 1 ms/ms; loss-percentage slopes use percentage points per second.

Each seed-level fit reports `r_squared_status` alongside its R²:

| Status | Interpretation |
| --- | --- |
| `defined` | At least two valid pairs, a varying predictor, and a finite R² |
| `constant_response` | At least two valid pairs and a varying predictor, with identical responses; R² is unavailable |
| `constant_predictor` | At least two valid pairs with an identical predictor; coefficients and R² are unavailable |
| `insufficient_samples` | Fewer than two valid pairs |
| `unavailable` | Both variables vary and the numerical R² is unavailable |

Seed-level slope intervals use the centered residual sum of squares with `n - 2` residual degrees of freedom and require at least three valid pairs.
The cross-seed intervals summarize the independently fitted slopes as described above.

Software-profiling summaries first average the internal repetitions within each invocation, then use independent invocations as the sample unit for confidence intervals.
Internal repetition rows remain available for inspecting run-to-run timing variation.

## Result files

Each run directory contains:

```text
run-manifest.json
run-start.json
execution-summary.json
processes/<family>/<run>.json
data/raw/<family>/<run>.out
data/raw/<family>/<run>.err
work/
```

The manifest records configuration, expanded commands, effective parameters, and experiment/analysis memberships.
The start record identifies executables, hashes, platform, library path, and selected runtime resources.
Each process record includes the command, start/end time, exit status, stream paths and hashes, and output checks.
Occupancy jobs additionally write `data/raw/occupancy/<run>.summary.csv`, `<run>.summary.json`, and `<run>.residence.csv` beside their raw streams.
Scenario telemetry is commonly on stderr; functional matrices and timing rows are commonly on stdout.
Both streams form the raw record.

Analysis produces:

| File | Contents |
| --- | --- |
| `records.json` | Per-run parameters, measurements, experiment memberships, observations, and process checks |
| `summary.csv` | One row per program, complete parameter combination, and metric, with sample counts and interval estimates |
| `paired.csv` | Enabled-minus-disabled paired feature effects |
| `regression.csv`, `regression.json` | Controller-delay slopes/intercepts and seed-level fits |
| `mechanism-observations.json` | Model-level observations and case outcomes |
| `issues.json` | Execution and data issues |

Structured CSV fields such as `parameters` and `experiments` contain JSON strings.
Unavailable estimates are empty CSV fields or JSON `null`, accompanied by their status.

### Measurements

Handover latency is the interval from scheduled producer handover to the first subsequent Interest received at its new attachment, reported in milliseconds.
The window-loss metric counts distinct sequences first requested in `[handover, handover + observation_window_s)` and the percentage still unsatisfied when the consumer stops at `stop_s - 2`.
All consumer transmissions for the same sequence belong to that sequence's record.
Signaling, route installation, completion, retry, withdrawal, and remembered-Interest forwarding are reported as event counters.

Interest transmissions through the normal FIB lookup path, including Trace Interests, are classified by the FIB that supplied the output port, with one count per egress transmission:

| Analysis field | Raw counter | Meaning |
| --- | --- | --- |
| `temporary_fib_interest_transmissions` | `temporaryFibInterestTransmissions` | Interest transmissions using the temporary FIB, including Trace Interests |
| `flow_table_fib_interest_transmissions` | `flowTableFibInterestTransmissions` | Interest transmissions using the flow-table FIB, including Trace Interests |
| `reforwarded_interests` | `pullsSent` | Direct sends of saved Interests after a route update |

The two FIB counters assign each normal egress transmission to exactly one route source; direct reforwarding has its own counter.
For archived raw records, `temporaryFibForwards` is read as `legacy_temporary_fib_matches`, the count of Interests selected by a temporary-FIB match, and `flowTableFibForwards` as `legacy_interest_transmissions`, the total ordinary Interest egress transmissions across both route sources.

Network-state telemetry samples the number of pending prefixes in each forwarder's local route controller (`m_seqMap.size()`), in entries.

| Program | Sampling cadence and start | Stop threshold | Recorded result |
| --- | --- | --- | --- |
| `statim-grid` | Every 10 ms, from `handover - 0.5 s` | `handover + controller_delay_s + 3 s` | Per-router `[TEMPORARY_FIB_PEAK]`; `temporary_fib_peak` in the analysis is the largest of the six routers' sampled maxima |
| `statim-path` | Every 5 ms, from `handover - 0.25 s` | `stop_s - 0.1 s` | Each router's sampled maximum is retained as `temporaryFibPeak` in raw `[PATH_NODE]` records |

Each sampler stops after the first tick at or after its threshold, or when the simulation ends.
The final tick can exceed the threshold by less than one sampling interval.
A zero sampled maximum means every sampled state was empty.
The separate occupancy experiment records its peak at every entry-arrival event and verifies the trajectory using its arrival and completion records.

Path telemetry records router sequences for the Trace Interest and Trace Data and the first steered request after handover.
`trace_round_trip_ms` measures the interval from the first router-side Trace Interest event to the last returning Trace Data update in the recorded handover path.

Occupancy residence records contain `seed`, `prefix_id`, `arrival_s`, `completion_s`, and `residence_s`, in simulated seconds.
Time-weighted mean occupancy is the integral of active entries divided by the observation horizon, including startup and final drain.
The occupancy integral is measured in entry-seconds and equals the sum of residence times.
Arrival rate multiplied by completion delay provides the zero-burst steady-state reference.

Software-profiling rows identify the operation, table size, hit outcome or Boolean update factors, repetition, operation count, counters, and validity checks.
`mean_ns` divides elapsed host time by operation count.
`p50_ns`, `p95_ns`, and `p99_ns` summarize per-operation batch averages.
Timing interpretation uses the recorded host and build conditions.

Keep the source revision, configuration, environment identity, process records, raw streams, and analysis report together when citing an experiment.
