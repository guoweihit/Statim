# 实验指南

[English](experiments.md) · [项目说明](../README.zh-CN.md) · [设计与实现](model.zh-CN.md)

实验研究生产者切换期间路由更新的生效时机，以及等待控制器安装时所需的临时状态。
本指南介绍运行环境、实验设置、测量指标和统计分析。

## 构建与首次运行

所提供的 ns-3 源码树已包含[基础版本说明](../BASELINE.zh-CN.md)中的固定依赖。
构建环境为 Linux x86-64、Ubuntu 18.04、GCC 7.5，Waf 使用 Python 2.7。
实验运行器使用 Python 3.6 或更高版本及其标准库。

在 Linux 或 WSL shell 中，从仓库根目录构建并启动项目环境：

```sh
docker build --platform linux/amd64 -f Dockerfile.environment -t statim-environment .
mkdir -p ../statim-results
docker run --rm -it --platform linux/amd64 \
  --mount type=bind,src="$(pwd)",dst=/code \
  --mount type=bind,src="$(cd ../statim-results && pwd)",dst=/results \
  --workdir /code statim-environment bash
```

在该环境中构建模型并进行快速检查：

```sh
python2.7 waf configure -d optimized --enable-modules=ndnSIM --disable-examples --disable-tests --disable-python
python2.7 waf -j8
python3 experiments/run.py --profile quick --output /results/quick --jobs 8
```

构建在 `build/src/ndnSIM/statim/` 下生成九个程序。
运行器定位所选程序并设置共享库搜索路径。
每次运行选择源码树之外的新结果目录。
写入 `/results` 的结果保留在宿主机的 `statim-results` 目录中。
`execution-summary.json` 给出完成数量和失败任务，各进程记录链接对应命令与日志。

使用完整种子集合和配置工作量时，执行：

```sh
python3 experiments/run.py --profile full --output /results/full --jobs 8
```

## 实验问题

| 实验组 | 目的与比较 |
| --- | --- |
| 切换质量 | 改变控制器安装时延和临时转发开关，并与 KITE-NFD 比较。在消费者重传开启和关闭时分别测量切换时延及未满足请求。 |
| 处理时延 | 为两个 Statim 转发配置增加相同的逐次处理时延，测量本地处理对切换和丢失率曲线的影响。 |
| Interest 再转发 | 在临时转发关闭时，沿更新后的下一跳再次发送已记录 Interest，测量该动作对恢复的作用。 |
| 完成通知丢失 | 在路由安装后丢弃完成通知，观察重试、临时状态清理和最终路由。 |
| 路径敏感性 | 改变路由器间链路数和消费者接入点，在所选控制时延下比较临时转发开启和关闭的情况。 |
| 占用 | 改变不同前缀的到达率、安装时延和初始突发，测量活跃临时条目及其驻留时间。 |
| 功能检查 | 检查控制事件次序与丢失、顺次查表、状态容器和报文流水线。 |
| 可选软件剖析 | 测量指定查找和更新操作在运行宿主机上的成本。 |

模型的报文及更新语义见[设计与实现](model.zh-CN.md)。
最终路由检查、同前缀替换和信令计数是移动实验的附加分析。
运行清单将每个唯一仿真实例关联到使用它的全部实验组和分析视角。

## 配置实验

[config.ini](../experiments/config.ini) 是带中英文注释的可编辑配置。
配置值使用 JSON 字面量：数值、`true`/`false`、带引号字符串及数组。
以 `#` 开头的整行注释解释单位、默认值、可选范围和参数关系。

`[defaults]` 定义完整的基础实验模板。
各 `[experiment.NAME]` 选择具体实验，并覆盖与模板不同的字段。
在实验节设置 `enabled = false` 可保留设置并暂停该组运行；`analysis_views` 可为结果添加描述性分析标签。
例如，`[experiment.handover_quality]` 继承完整比较，处理时延组选择较小的控制时延扫描及附加处理时间。
各实验注释说明参数选择的科学目的。
基础模板提供继承值，只有具名实验节生成实际执行的组。
其他场景的字段继续保存在继承模板中，各场景只把适用字段传给相应程序。
显式覆盖的字段须适用于所选场景和系统；配置加载器同时校验已启用和已禁用的实验节。

| 设置 | 含义 |
| --- | --- |
| `systems` | `statim` 选择 Statim 转发模型，`nfd-kite` 选择固定的 KITE-NFD 协议栈。 |
| `temporary_forwarding` | 对 Statim，`true` 使用匹配的本地临时路由，`false` 使用控制器安装的路由进行转发。两种设置均执行临时更新和控制交互。 |
| `interest_reforwarding` | 控制路由更新后对已记录 Interest 的再转发。 |
| `consumer_retransmissions` | 控制消费者对尚未满足序列的重复发送。 |
| `controller_delay_s` | Statim 控制器服务的决策到安装时延，单位为仿真秒。 |
| `seed_first`、`seed_last` | 包含首尾的独立种子区间。 |

数组展开为实验的一项维度。
NFD-KITE 参照使用自身的本地路由更新路径。
其运行可改变共同的移动、流量、消费者重传和 Interest 再转发参数。
控制时延、临时转发、处理时延和重试策略属于 Statim 的实验维度。
一组实验同时选择两个系统时，这些维度只展开 Statim 的运行，同一份 NFD-KITE 观测用于相应对照。
运行清单记录各次运行实际生效的参数。
程序、有效参数和种子相同的配置共享一次执行，其全部用途保留在清单中。

### 场景与参数适用范围

| `scenario` | 程序与拓扑 | 可用系统 |
| --- | --- | --- |
| `handover` | `statim-grid` 或 `kite-grid`：六个路由器，汇合点接 R1，消费者接 R4，生产者从 R3 移动至 R6 | `statim`、`nfd-kite` |
| `path` | `statim-path`：R0 至 RH 的链，汇合点接 R0，生产者旧接入点位于 R0 的分支，新接入点为 RH | `statim` |
| `occupancy` | `temporary-fib-occupancy`：通过 Statim 状态更新路径执行唯一前缀的到达及控制器完成 | `statim` |

两个切换程序均按每跳接口成本为 1 计算路由。
共用的[网格拓扑](../src/ndnSIM/statim/scenarios/common/grid-topology.txt)记录这一单位成本，并固定两个程序使用的路由器及链路顺序。

下表覆盖 `[defaults]` 中的全部字段。
秒均指仿真时间；`stateful_delay_us` 使用微秒。

| 字段 | 适用范围与含义 |
| --- | --- |
| `scenario`、`systems`、`seed_first`、`seed_last` | 全部场景。种子区间包含首尾，范围为 1 至 4294967295。移动程序设置 ns-3 的 `RngRun`；占用程序设置 `RngSeed`，并将 run 设为 1。 |
| `consumer_retransmissions`、`interest_reforwarding` | 两个移动场景及其可用系统。这是相互独立的布尔选项。 |
| `consumer_cbr_frequency`、`payload_bytes` | 两个移动场景及其可用系统。分别为正数请求发送率（次/秒）和非负 Data 载荷字节数。 |
| `handover_base_s`、`handover_jitter_s`、`observation_window_s`、`stop_s` | 两个移动场景及其可用系统。切换时刻为 `base + U[0, jitter]`，观测窗口从该时刻开始。 |
| `refresh_interval_s`、`trace_lifetime_s`、`interest_lifetime_s`、`initial_rtt_estimate_s` | 两个移动场景及其可用系统。依次为 Trace 发送间隔、学习路由寿命、Interest 报文寿命及消费者初始 RTT 估计。 |
| `temporary_forwarding`、`stateful_delay_us` | 仅 Statim 移动场景。分别控制临时路由转发和模块的附加处理时延。 |
| `controller_delay_s` | Statim 移动场景中为非负的决策至安装时延 Tc；占用场景中为每个到达前缀的正完成时延 Tc。 |
| `interest_reforwarding_limit`、`generation_guard` | 仅 Statim 的 `handover`。每次接受更新后再次发送的已记录 Interest 数量上限，以及持久代次检查开关。 |
| `completion_notice_loss`、`controller_retry_limit` | 仅 Statim 的 `handover`。完成通知丢失概率（0 至 1），以及首次请求之后最多追加的尝试次数。 |
| `controller_retry_timeout_s`、`retry_timeout_margin_s` | 仅 Statim 的 `handover`。超时可取非负秒数或 `"auto"`；自动模式在通知丢失概率为正时取 `2 * controller_delay_s + retry_timeout_margin_s`，否则取零。裕量仅参与自动计算。零超时表示按完成事件处理。 |
| `trace_path_hops`、`consumer_join` | 仅 `path`。H 为至少 2 的路由器间链路数；`"rendezvous"` 将消费者接到 R0，`"middle"` 接到编号为 `floor(H/2)` 的路由器。 |
| `arrival_rate_per_s`、`arrival_count`、`initial_burst` | 仅 `occupancy`。分别为正数指数到达率、正整数前缀总数，以及零时刻同时到达的前缀数。初始突发计入总数，范围为零至 `arrival_count`。 |

路径程序固定采用 `interest_reforwarding_limit=1000`、`generation_guard=false`、`controller_retry_limit=3`、`controller_retry_timeout_s=0` 和 `completion_notice_loss=0`。
其运行清单记录这些值；如需改变该控制策略，选择 Statim 的 `handover` 场景。
占用场景在最后一次到达完成后结束，观测时长由到达负载和 Tc 决定。
网络流量及移动窗口设置适用于两个移动场景。

允许使用数组的维度包括 `systems`、`consumer_retransmissions`、`interest_reforwarding`、`temporary_forwarding`、`controller_delay_s`、`stateful_delay_us`、`completion_notice_loss`、`trace_path_hops`、`consumer_join`、`arrival_rate_per_s` 和 `initial_burst`。
它们接受单值或元素互异的非空数组；其他字段接受单值。
所有数值须为有限值。
整数计数及数量上限须能用 32 位无符号数表示；`payload_bytes`、`interest_reforwarding_limit` 和 `controller_retry_limit` 使用有符号整数，最大为 2147483647。

Interest 寿命和初始 RTT 估计值是两个独立设置。
对应配置键为 `interest_lifetime_s` 和 `initial_rtt_estimate_s`。
前者决定请求携带的寿命，后者初始化消费者的重传超时估计器。
两者默认均为 2 秒，可分别调整。
Interest 和 Trace 寿命至少为 0.001 秒，且采用整毫秒，例如 0.001、0.250 或 2。
初始 RTT 估计接受有限正值，并支持仿真器时间精度内的亚毫秒值。
生产者的 Trace 刷新间隔和跟踪寿命分别控制信令发送节奏及学习路由的寿命。
主对比为两种实现设置5秒路由寿命和2秒刷新间隔。
在最大2秒控制时延下，这满足稳定路径的租约交叠条件“寿命大于刷新间隔加控制时延”。
Statim 在本地接纳时固定路由的绝对截止时间；控制器安装剩余有效期，并拒绝在截止时刻或之后生效的更新。
`route_expiry_boundary` 组采用2秒寿命、2秒刷新间隔，以及0、0.5、2秒控制时延，比较双FIB开关，并关闭消费者重传和Interest再次转发。
过期安装和未观测到切换完成的情况均作为边界结果保留。
它们与控制时延的关系决定完成通知到达时，更新是否仍为当前有效代次。

消费者从 1 秒发送至 `stop_s - 2`；生产者在 `stop_s - 1` 停止。
消费者在停止时结算请求满足计数，仿真继续执行至 `stop_s`，以收集控制器和转发器的最终状态。
时间设置满足 `handover_base_s >= 1`、抖动非负，以及 `handover_base_s + handover_jitter_s + observation_window_s < stop_s - 2`。
观测窗口和结束时间均为正。
路径隔离实验还要求 Trace 寿命和刷新间隔均大于 `stop_s`，使被测切换只包含一轮信令。
`[checks]` 选择独立功能程序，各程序执行一次。

`full` 使用全部所选种子和配置的工作量。
`quick` 使用每个参数单元的首个种子，并缩减占用及软件剖析的工作量。
快速占用实验保留所配置的初始突发；完整工作量含后续到达时，也至少保留一个后续到达。
展开后的实际规模见运行清单中的 `process_count` 和 `counts`。

使用独立配置文件：

```sh
cp experiments/config.ini /path/to/my-experiments.ini
python3 experiments/run.py --config /path/to/my-experiments.ini --profile full --output /results/custom
```

例如，在副本中编辑原有的切换质量实验节，选择十个种子和三个 Tc 值：

```ini
[experiment.handover_quality]
seed_first = 101
seed_last = 110
controller_delay_s = [0, 0.1, 0.5]
```

这些覆盖只改变该组实验；其他已启用的实验组保留各自设置。
修改 `[defaults]` 可改变各组继承的值，在各实验节设置 `enabled = false` 可缩小执行范围。
`full` 执行全部十个种子；`quick` 在此组中执行种子 101。

### 运行器选项

| 选项 | 默认值 | 用途 |
| --- | --- | --- |
| `--config PATH` | `experiments/config.ini` | 带注释的实验设置 |
| `--profile {full,quick}` | `full` | 种子和工作量配置 |
| `--output PATH` | 必填 | 源码树之外的新输出目录 |
| `--jobs N`、`-j N` | `8` | 并发仿真进程数 |
| `--timeout SECONDS` | `300` | 每个进程的墙钟时间上限 |
| `--microbenchmark` | 使用配置值 | 开启可选的宿主机软件剖析 |
| `--no-microbenchmark` | 使用配置值 | 关闭可选的宿主机软件剖析 |
| `--cpu N` | 首个允许的逻辑 CPU | 串行软件剖析所用的 CPU |

所提供的配置关闭软件剖析。
在 `[microbenchmark]` 中保持 `enabled = false`，或通过 `--no-microbenchmark` 在本次运行中关闭：

```sh
python3 experiments/run.py --profile full --no-microbenchmark --output /results/full
```

采集宿主机计时数据时可显式开启：

```sh
python3 experiments/run.py --profile full --microbenchmark --output /results/full-with-profiling
```

`[microbenchmark]` 指定操作数量、批次大小、每次进程调用内的重复次数，以及独立进程调用次数。
计时进程在全部仿真完成后串行运行。
请随计时结果记录宿主机负载、频率条件、镜像身份和 CPU 选择。

也可通过 Waf 单独运行检查：

```sh
python2.7 waf --run state-table-tests
python2.7 waf --run statim-packet-pipeline-tests
python2.7 waf --run "statim-grid --PrintHelp"
```

`checks.programs` 可选择 `state-table-tests`（状态容器案例）、`statim-packet-pipeline-tests`（报文编码、流水线、交换机、缓存和消费者计数器）、`lpm-consistency`（临时表优先与合并两表的查找选择）及 `statim-controller-faults`（控制事件案例）。
控制事件案例覆盖正常完成、完全相同的重复更新、同代次冲突载荷、旧请求/安装/完成通知迟到，以及三个控制环节分别发生单次或持续丢失。
另外八项检查覆盖共同到期、安装时到期、控制请求到达前到期、重试保持截止时刻、完成通知晚于到期、同代截止时刻冲突、新代刷新获得新截止，以及安装拒绝后不产生成功通知。

## 分析运行结果

分析使用独立宿主环境中的 Python 3.10 或更高版本，以及 NumPy 和 SciPy。
在仓库根目录执行：

```sh
python3 -m pip install -r requirements-analysis.txt
python3 experiments/analyze.py \
  --run-root ../statim-results/full \
  --output ../statim-results/analysis/report.json \
  --verify
```

分析器按运行清单记录的有效参数分组，因此完整、快速和自定义配置使用同一入口。
主 JSON 报告写到 `--output`，各表写到去掉该路径后缀所得的同级目录，例如 `report.json` 对应 `report/`。
可用 `--tables PATH` 显式选择空的表目录。
`--confidence` 选择双侧置信水平，默认为 0.95。
`--verify` 还核对记录的文件哈希及逐条占用驻留记录。
`--data-root PATH` 添加与历史运行目录或先前生成的 `records.json` 的比较。

`issues.json` 汇总执行和数据完整性错误。
`mechanism-observations.json` 单独报告模型观测，其中包括专门构造的不同查表结果案例。
运行中包含可选实验组时，分析产生对应统计行。

### 统计单位与比较

仿真汇总的统计单位是固定有效参数组合下的一个独立种子。
分析器按全部非重复参数分组，记录实际样本数，并使用 SciPy 的分位数函数及 `n - 1` 个自由度计算 Student-t 置信区间。
汇总中的 `total` 为成功加载并处理重复种子后的记录数，`n` 为有限测量值数量，`missing` 为缺失值数量，`invalid` 为非有限值或格式错误值数量。
`n=0` 时均值、标准差和区间均不可估计。
`n=1` 时均值为该观测值，标准差和区间不可估计。
这两种情况均使用 `ci_status=insufficient_samples`。
两个及以上的相同样本产生零宽区间，标记为 `zero_sample_variance`。
执行失败列在 `issues.json` 和主报告中；`total` 统计成功加载的记录，因此解释指标计数时须一并查看这些错误。
一组记录全部缺少某指标时，该指标不产生汇总行。

功能开关比较对相同种子、其他有效参数完全相同的开启和关闭记录实施配对。
报告的差值为开启减关闭。
`missing_metric_pairs`、`unmatched_seeds` 和 `ambiguous_seeds` 分别记录缺指标、未匹配及存在歧义的比较。
控制时延分析先在每个种子的时延扫描中拟合直线，再跨种子汇总斜率和截距。
这保留了同一种子跨时延点复用所形成的配对关系。
自变量使用秒，因此切换时延斜率 1000 毫秒/秒对应 1 毫秒/毫秒；丢失率斜率使用百分点/秒。

逐种子拟合同时给出 R² 及 `r_squared_status`：

| 状态 | 含义 |
| --- | --- |
| `defined` | 至少两个有效配对，自变量有变化，R² 为有限值 |
| `constant_response` | 至少两个有效配对，自变量有变化、响应完全相同，R² 不可用 |
| `constant_predictor` | 至少两个有效配对，自变量完全相同，系数和 R² 均不可用 |
| `insufficient_samples` | 有效配对少于两个 |
| `unavailable` | 两个变量均有变化，数值计算所得 R² 不可用 |

逐种子斜率区间采用中心化残差平方和与 `n - 2` 个残差自由度，要求至少三个有效配对。
跨种子区间按照前述方法汇总各独立种子拟合出的斜率。

软件剖析先汇总每次独立进程调用内的重复均值，再以独立调用为置信区间的样本单位。
内部重复明细继续保留，供检查计时变化。

## 结果文件

每次运行的目录包含：

```text
run-manifest.json
run-start.json
execution-summary.json
processes/<family>/<run>.json
data/raw/<family>/<run>.out
data/raw/<family>/<run>.err
work/
```

清单记录配置、展开命令、有效参数及实验和分析归属。
启动记录标识可执行文件、哈希、平台、库路径和所选运行资源。
各进程记录包括命令、起止时间、退出状态、输出流路径与哈希，以及输出检查。
占用任务还在原始输出旁写入 `data/raw/occupancy/<run>.summary.csv`、`<run>.summary.json` 和 `<run>.residence.csv`。
场景遥测通常写入 stderr，功能矩阵与计时记录通常写入 stdout。
两个输出流共同构成原始记录。

| 文件 | 分析输出 |
| --- | --- |
| `records.json` | 逐运行参数、测量、实验归属、观测及进程检查 |
| `summary.csv` | 每个程序、完整参数组合和指标对应一行，含样本数和区间估计 |
| `paired.csv` | 功能开启减关闭的配对效应 |
| `regression.csv`、`regression.json` | 控制时延斜率、截距和逐种子拟合 |
| `mechanism-observations.json` | 模型观测和案例结果 |
| `issues.json` | 执行和数据问题 |

CSV 中的 `parameters`、`experiments` 等结构字段使用 JSON 字符串。
无法估计的值以空 CSV 字段或 JSON `null` 表示，并附状态。

### 测量定义

切换时延从预定生产者切换时刻起算，到其新接入点首次收到后续 Interest 为止，单位毫秒。
窗口丢失指标统计首次请求发生在 `[切换时刻, 切换时刻 + observation_window_s)` 内的不同序列，以及其中在消费者于 `stop_s - 2` 停止时仍未满足的百分比。
消费者对同一序列的全部发送属于同一份序列记录。
信令、路由安装、完成、重试、撤回和已记录 Interest 的再转发使用事件计数器报告。

经常规 FIB 查找路径转发的 Interest（包括 Trace Interest）按提供输出端口的 FIB 分类，每次出口发送计一次：

| 分析字段 | 原始计数器 | 含义 |
| --- | --- | --- |
| `temporary_fib_interest_transmissions` | `temporaryFibInterestTransmissions` | 使用临时 FIB 的 Interest 发送次数，包括 Trace Interest |
| `flow_table_fib_interest_transmissions` | `flowTableFibInterestTransmissions` | 使用流表 FIB 的 Interest 发送次数，包括 Trace Interest |
| `reforwarded_interests` | `pullsSent` | 路由更新后直接发送已保存 Interest 的次数 |

常规查表路径的每次出口发送恰好归入一个 FIB 来源；直接再转发使用独立计数器。
对于已归档的原始记录，`temporaryFibForwards` 读为 `legacy_temporary_fib_matches`，统计由临时 FIB 匹配选中的 Interest 数量；`flowTableFibForwards` 读为 `legacy_interest_transmissions`，统计两种路由来源的普通 Interest 出口发送总次数。

网络状态遥测对各转发器本地路由控制器中的待确认前缀数（`m_seqMap.size()`）进行采样，单位为条目。

| 程序 | 采样周期与起点 | 停止阈值 | 记录结果 |
| --- | --- | --- | --- |
| `statim-grid` | 每 10 毫秒一次，从 `切换时刻 - 0.5 秒` 开始 | `切换时刻 + controller_delay_s + 3 秒` | 各路由器输出 `[TEMPORARY_FIB_PEAK]`；分析中的 `temporary_fib_peak` 取六个路由器采样最大值中的最大者 |
| `statim-path` | 每 5 毫秒一次，从 `切换时刻 - 0.25 秒` 开始 | `stop_s - 0.1 秒` | 各路由器的采样最大值保存在原始 `[PATH_NODE]` 记录的 `temporaryFibPeak` 字段中 |

采样在首次达到或超过阈值后停止，仿真结束也会停止采样。
最后一次采样可比阈值晚不足一个采样周期。
采样最大值为零表示每个采样时刻的状态均为空。
独立占用实验在每次条目到达事件记录峰值，并使用到达及完成记录校验轨迹。

路径遥测记录 Trace Interest 和 Trace Data 经过的路由器序列，以及切换后首次被引导的请求。
`trace_round_trip_ms` 测量所记录切换路径中从首个路由器侧 Trace Interest 事件至最后一次返回的 Trace Data 更新之间的间隔。

占用驻留记录包含 `seed`、`prefix_id`、`arrival_s`、`completion_s` 和 `residence_s`，时间单位为仿真秒。
时间加权平均占用等于活跃条目数的时间积分除以观测时长，包含启动和最终排空阶段。
占用积分单位为条目·秒，等于全部驻留时间之和。
到达率乘以完成时延给出零突发时的稳态参照值。

软件剖析记录标识操作、表规模、命中结果或布尔更新因素、重复编号、操作数、计数器和有效性检查。
`mean_ns` 将宿主机耗时除以操作数。
`p50_ns`、`p95_ns` 和 `p99_ns` 汇总逐批次的每操作平均耗时。
计时解释以记录的宿主机和构建条件为依据。

引用实验时，一同保留源码版本、配置、环境身份、进程记录、原始输出及分析报告。
