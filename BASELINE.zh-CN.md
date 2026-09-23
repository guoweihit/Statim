# 构建环境与依赖版本

[English](BASELINE.md)

有线仿真基础代码包含以下公开源码版本：

| 组件 | 公开仓库 | 提交 |
| --- | --- | --- |
| ns-3 | https://github.com/named-data-ndnSIM/ns-3-dev | `333e6b052c101625199af40107edd6e379a36119` |
| ndnSIM | https://github.com/KITE-2018/ndnSIM | `a97ff1ada6f0fa3ed9ec1c0e1719609562d9643b` |
| NFD | https://github.com/KITE-2018/NFD | `c0f0123e6a461b6154ffe5feb9554fcc7345bd5b` |
| ndn-cxx | https://github.com/named-data-ndnSIM/ndn-cxx | `4692ba80cf1dcf07acbbaba8a134ea22481dd457` |

构建选择有线 ns-3 模块及其依赖。
KITE 应用使用有线拓扑接口。
各组件的 `VERSION` 文件标识固定的 NFD 和 ndn-cxx 版本。

保留的上游文档供查阅组件和接口说明。
上表所列仓库及版本提供完整的上游教程示例、拓扑文件和绘图输入；本仓库提供的 Statim 构建与复现流程见[实验指南](docs/experiments.zh-CN.md)。

构建环境为 Linux x86-64、Ubuntu 18.04、GCC 7.5、Python 2.7、Boost 1.65、OpenSSL、SQLite 和 Crypto++。
[Dockerfile.environment](Dockerfile.environment)指定 Ubuntu 镜像摘要并安装开发包。
环境提供 UTF-8 区域设置。
科学计算分析使用独立的 Python 3.10 或更高版本环境，NumPy/SciPy 版本范围见 [requirements-analysis.txt](requirements-analysis.txt)。

在本仓库根目录执行：

```sh
python2.7 waf configure -d optimized --enable-modules=ndnSIM --disable-examples --disable-tests --disable-python
python2.7 waf -j8
```

优化编译配置和模块选择构成本项目的实验构建设置。
各组件的原始许可及作者记录见[第三方软件](THIRD-PARTY.zh-CN.md)。
