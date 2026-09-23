# Statim simulation artifact

[中文](README.zh-CN.md)

Statim studies data-plane-initiated routing during producer mobility in Named Data Networking (NDN).
It applies routing decisions locally while a controller installs the corresponding routes asynchronously.
This repository contains the software forwarding model, wired ns-3/ndnSIM scenarios, functional checks, optional software profiling, and experiment tools.

Start with the [experiment guide](docs/experiments.md) for building, configuring, running, and analyzing the experiments.
[Design and implementation](docs/model.md) explains the forwarding and update paths.

| Path | Contents |
| --- | --- |
| `src/ndnSIM/statim/` | Statim model, applications, scenarios, and checks |
| `experiments/config.ini` | Annotated experiment template and per-experiment settings |
| `experiments/` | Run and analysis programs |
| `docs/` | Design description and experiment guide |

[BASELINE](BASELINE.md) records the build environment and fixed dependency versions.
[Third-party software](THIRD-PARTY.md) lists source locations and original notices.
[LICENSE](LICENSE.md) states the terms for Statim contributions.
