# @YOKE_ID@

A Yoke Plugin written in C.

| Path | What it holds |
| --- | --- |
| `yoke-plugin.yaml` | the file every step of `yoke-compose` reads: the identity, the build, the architectures |
| `plugin/` | the Plugin's source, and its Manifest, `@YOKE_ID@.yaml`, kept beside it |
| `try/` | an application project that uses the Plugin, to see it work |

Build it with `yoke-compose build`; publish it by tagging this repository `vX.Y.Z`.

The library is found where it is installed, or fetched and built beside the Plugin; either way its build
needs CMake, pkg-config and the headers of protobuf-c, gRPC's core C API, json-c and OpenSSL — on Debian,
`cmake pkg-config libprotobuf-c-dev libgrpc-dev libjson-c-dev libssl-dev`. Building for another
architecture than this machine's needs a CMake toolchain file, named by `CMAKE_TOOLCHAIN_FILE`.
