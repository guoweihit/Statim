# Build environment and dependency versions

[中文](BASELINE.zh-CN.md)

The wired simulation foundation includes these public source revisions:

| Component | Public repository | Commit |
| --- | --- | --- |
| ns-3 | https://github.com/named-data-ndnSIM/ns-3-dev | `333e6b052c101625199af40107edd6e379a36119` |
| ndnSIM | https://github.com/KITE-2018/ndnSIM | `a97ff1ada6f0fa3ed9ec1c0e1719609562d9643b` |
| NFD | https://github.com/KITE-2018/NFD | `c0f0123e6a461b6154ffe5feb9554fcc7345bd5b` |
| ndn-cxx | https://github.com/named-data-ndnSIM/ndn-cxx | `4692ba80cf1dcf07acbbaba8a134ea22481dd457` |

The build selects the wired ns-3 modules and their dependencies.
KITE applications use the wired topology interface.
Component `VERSION` files identify the fixed NFD and ndn-cxx revisions.

The retained upstream documentation provides component and API reference material.
The repositories and revisions above provide the complete upstream tutorial examples, topology files, and plotting inputs; [the experiment guide](docs/experiments.md) covers the Statim build and reproduction workflow supplied here.

The build environment is Linux x86-64, Ubuntu 18.04, GCC 7.5, Python 2.7, Boost 1.65, OpenSSL, SQLite, and Crypto++.
[Dockerfile.environment](Dockerfile.environment) specifies the Ubuntu image digest and installs the development packages.
The environment provides UTF-8 locale support.
Scientific analysis runs separately with Python 3.10 or later and the NumPy/SciPy versions specified in [requirements-analysis.txt](requirements-analysis.txt).

From this repository root, build with:

```sh
python2.7 waf configure -d optimized --enable-modules=ndnSIM --disable-examples --disable-tests --disable-python
python2.7 waf -j8
```

The optimized profile and module selection are the project's experiment build settings.
Original component licenses and authorship records are indexed in [Third-party software](THIRD-PARTY.md).
