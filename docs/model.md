# Statim design and implementation

[中文](model.zh-CN.md) · [README](../README.md) · [Experiment guide](experiments.md)

Statim lets a forwarder apply a routing decision locally while a controller installs the corresponding route asynchronously.
This repository implements that design in ns-3/ndnSIM and applies it to producer mobility with KITE.
The experiment configuration selects the forwarding behavior, traffic, control delay, and observation interval.

## Forwarding path

A Statim forwarder combines a match-action pipeline with a stateful software module.
The pipeline passes an arriving packet to the module for NDN processing.
For an Interest, the module records pending state and checks the content store.
A content-store hit returns Data immediately.
An accepted Interest with a cache miss receives the configured processing delay and proceeds to route lookup.

The module keeps a temporary FIB for locally accepted routing decisions.
When temporary forwarding is enabled, a matching entry supplies the outgoing ports.
Other Interests return to the pipeline, which looks up the controller-installed route in its flow-table FIB.
The output-port bitmap carries a module decision through the pipeline.
Ordinary Interest forwarding removes the ingress port from the outgoing set.

Received Data passes through the configured processing delay, satisfies matching pending records, and returns through their deduplicated ingress ports.
KITE Trace Data also triggers a routing update: its traced prefix identifies the destination, and the matching PIT record supplies the next hop toward the producer.
The endpoint applications generate KITE signaling and measure requests received after the scheduled producer handover.

## Route updates and controller completion

The local route controller accepts increasing generations for a prefix.
An accepted update replaces that prefix's previous temporary next hop and submits a control request.
The saved Trace Interest supplies the route lifetime L; admission at simulated time t fixes the deadline t+L in both the temporary entry and the control message.
The flow-table controller schedules installation after the configured delay and applies an update while its deadline is still in the future.
An accepted installation uses that original deadline and produces a completion notification, `SyncAck`.
A notification matching the current pending generation and deadline removes the temporary entry while the installed route remains valid.
Both tables treat the route as expired at its deadline.

The primary mobility configuration studies this normal update path.
Additional controls enable persistent generation checks, timeout-based retries, and independent loss or delay on the request, installation, and completion legs.
With generation checks enabled, the controller retains the highest accepted and applied generation for each prefix, rejects stale updates and conflicting same-generation payloads, and acknowledges an identical completed retry.
After the retry budget is exhausted, the local controller withdraws the temporary entry and its pending request.
Every retry carries the original deadline, and an installed flow-table route retains that same deadline after temporary-state withdrawal.
The Statim handover scenario exposes these policy settings, and the standalone control program exercises loss and event-order cases.
The path scenario uses a fixed control policy, recorded with its effective parameters in the run manifest.
The [experiment guide](experiments.md) lists each setting's scope and the available functional checks.

Interest reforwarding is a separate option.
After accepting a routing update, Statim enumerates saved Interests under the updated prefix in name order, refreshes their nonces, and schedules direct sends to the new next hop at the current simulated time, up to the per-update limit.
Eligibility follows the stored pending records, including records whose ingress lifetimes have elapsed since the last aggregation.
The KITE-NFD reference selects matching PIT entries with a live ingress record and with the new face absent from their ingress and outgoing records.
It sends the saved Interest with its original nonce and packet lifetime, scheduling successive candidates at 1 ms offsets.
These settings exercise the reforwarding behavior implemented by each stack.
Consumer retransmission separately controls whether the consumer sends an outstanding sequence again.

## State and lookup semantics

The temporary and pending tables use `std::map` keyed by complete NDN names.
The local controller owns update ordering and single-next-hop replacement policy; the temporary table supplies storage and longest-prefix lookup.

| Object | Implemented behavior |
| --- | --- |
| Pending Interest table | Saves the first accepted Interest for a name and records ingress ports with individual insertion times and lifetimes. Duplicate detection compares the arriving nonce with the first saved nonce. Accepted aggregation prunes expired ingress records. Data satisfaction consumes stored ancestor-name matches, including exact name and root, and returns their ports. Prefix enumeration provides saved Interests in name order. |
| Temporary FIB | Holds ports and deadlines for each prefix. Insertion prunes expired hops at that prefix and adds an absent port; an existing live port keeps its deadline. Lookups prune expired hops and can fall back to a shorter live prefix. `size()` counts stored prefixes, with cleanup performed during insertion and lookup. |
| Flow-table FIB | Matches the encoded name fields in priority order. More represented prefix components give higher priority. The controller installs or replaces the route and its timeout. |
| Content store | Uses exact-name lookup and an entry-count bound. Eviction first applies a coin-flip scan, then removes entries in container order until the bound is met. Capacity zero selects operation with caching disabled. |

Data satisfaction and pending-Interest enumeration use the records stored at invocation; accepted Interest aggregation performs pending-record lifetime pruning.
Those stored records can include ingress records whose lifetimes elapsed since the last accepted aggregation.
The state containers use the ndn-cxx steady clock supplied by ndnSIM, and their unit checks inject a controlled clock.

A temporary match takes precedence over a flow-table match.
For example, for `/alpha/item/segment`, a temporary route `/alpha` is selected ahead of a flow-table route `/alpha/item`.
The lookup tests include this counterexample to longest-prefix selection across the union of both tables, along with five cases where the selections agree.
This precedence implements a temporary override for the affected prefix.

## Packet representation

A 12-byte simulation header precedes the NDN TLV block:

| Field | Width | Use |
| --- | ---: | --- |
| Packet type | 1 byte | Processing stage and packet class |
| Output ports | 4 bytes | Ports 1 through 32; port 1 occupies the most significant bit |
| Input port | 2 bytes | Stateful-module ingress port |
| Name hashes | 4 bytes | One 8-bit hash for each of the first four name components |
| Switch ID | 1 byte | Switch metadata and Data hop-count propagation |

Each component URI is hashed with `std::hash<std::string>` modulo 254; `0xff` represents a wildcard.
Names sharing the represented hash tuple share a flow-match representation.
The experiment therefore uses one recorded compiler/library environment across the simulated nodes.
State containers accept 16-bit port numbers, while packet output actions use the 32-port bitmap.
Match fields use bit offsets; set-field actions use byte offsets.

## Simulation assumptions

The mobility scenarios use wired links, scheduled attachment changes, one mobile producer, and a trusted signaling path.
The controller service expresses decision-to-installation delay through event scheduling.
Processing-delay sensitivity uses an independent timer for each accepted cache-miss Interest and each received Data packet.
These timers can overlap across packets.
The supplied 6744-microsecond setting is an added-latency input for this timer.
The flow-table pipeline, pending state, route updates, and counters execute as software within that event model.

The occupancy scenario inserts unique prefixes through the Statim state-update path, with exponentially distributed inter-arrival times after an optional initial burst.
These synthetic updates remain valid until controller completion; zero in their internal deadline field denotes this unbounded lease.
Every accepted update completes after the configured deterministic delay.
Its observation horizon ends at the last arrival plus that delay.
Time-weighted occupancy includes startup and final drain; the zero-burst steady-state reference is the arrival rate multiplied by mean residence time.
The reported occupancy counts active temporary forwarding entries, and persistent generation records form a separate resource category.

Optional software timing measures selected host operations using `std::chrono::steady_clock`.
Its lookup fixtures exercise temporary-FIB longest-prefix matching and serialized flow-table lookup.
Its update fixtures vary generation bookkeeping, temporary-prefix replacement, and control-message construction/callback dispatch.
Names and hashes are prepared before timing, and batch averages reduce timer-call overhead.
These results describe the selected host, compiler, containers, and synthetic inputs.

## Source map

Paths below are relative to `src/ndnSIM/statim/`.

| Path | Contents |
| --- | --- |
| `forwarding-engine.*`, `stateful/` | Packet processing, KITE updates, and remembered-Interest forwarding |
| `state/` | Pending Interest and temporary forwarding tables |
| `control/` | Local route generations, controller installation, completion, and retries |
| `pipeline/`, `packet-header.*` | Header encoding, match-action processing, and output ports |
| `apps/`, `helper/`, `scenarios/` | Endpoint traffic, topology, mobility, and scenario parameters |
| `tests/` | State, packet, lookup, control-event, and occupancy checks |
| `benchmarks/` | Optional host software timing |

The KITE-NFD comparison uses the fixed public implementation listed in [BASELINE](../BASELINE.md).
That revision has a trace table and `TraceForwardingStrategy`; the adapters under Statim's `apps/` provide the corresponding application behavior for the Statim stack.
Integration changes connect the model through ndnSIM packet, transport, stack, and routing-helper interfaces.
KITE application changes expose experiment controls and request measurements, and the NFD change records remembered-Interest forwarding.
Adapted files preserve their upstream notices and identify local modifications; the Trace-name utility in `packet-utils.hpp` retains its NFD notice.
Experiment choices, parameter relationships, measurement definitions, and analysis commands are in the [experiment guide](experiments.md).
