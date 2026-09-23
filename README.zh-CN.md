# Statim 仿真实验代码

[English](README.md)

Statim 研究命名数据网络（Named Data Networking，NDN）中生产者移动时由数据平面发起的路由更新。
它在本地应用路由决策，同时由控制器异步安装对应路由。
本仓库包含软件转发模型、有线 ns-3/ndnSIM 场景、功能检查、可选的软件性能剖析和实验工具。

从[实验指南](docs/experiments.zh-CN.md)开始，完成构建、配置、运行和分析。
[设计与实现](docs/model.zh-CN.md)介绍转发及更新路径。

| 路径 | 内容 |
| --- | --- |
| `src/ndnSIM/statim/` | Statim 模型、应用、场景和检查 |
| `experiments/config.ini` | 带注释的基础模板和各组实验设置 |
| `experiments/` | 运行与分析程序 |
| `docs/` | 设计说明和实验指南 |

[基础版本说明](BASELINE.zh-CN.md)记录构建环境和固定依赖版本。
[第三方软件](THIRD-PARTY.zh-CN.md)列出源码位置及原始声明。
[许可说明](LICENSE.md)给出 Statim 贡献的使用条款。
