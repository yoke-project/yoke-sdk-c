# yoke-sdk-c

## What it is

The C family of Yoke's SDKs: the library a C program uses to run as a Plugin unit in a Yoke
instance, the library an interface client attaches to a channel with — on a local socket, or over TLS
1.3 at a routable address — over a base they share, and the harnesses that hold each to its contract
under the conformance suite. It speaks gRPC through gRPC's core C API, its messages are generated with
protobuf-c, and its TLS is OpenSSL's.

It is not the Core, and it holds no contract definitions of its own: the C it builds on is generated
from the published definitions, at the version `definitions/record` pins. It is released as its
source: an archive of the tagged tree, which `cmake --install` turns into the library, its headers, a
CMake package config for `find_package(yoke-sdk-c)` and a `yoke-sdk-c.pc` for `pkg-config`. No package
manager is involved.

## What is where

| Path | What it holds |
| --- | --- |
| `include/yoke/` | The public headers: `base.h`, `plugin.h` and `interface.h` |
| `base/` | What the libraries share: the connection, the envelope, a refusal |
| `plugin/` | The library a Plugin unit is written with |
| `interface/` | The library an interface client is written with, and the TLS 1.3 it carries itself |
| `harness/` | The plugin library's harness for the conformance suite |
| `interface-harness/` | The interface library's harness |
| `definitions/` | The generated C of the plugin and interface contracts, and `record`, which pins the definitions' version, digest and generators |
| `testing/` | What the programs that run the cases share |
| `cmake/` | The package config and the pkg-config file `cmake --install` writes |
| `checks/`, `ci/` | This repository's own checks, and the scripts continuous integration runs; `ci/definitions.sh` regenerates `definitions/` |

A test's description sits beside the test, as a `.std.md` file.

## Working on it

A C compiler, CMake and `pkg-config`; the development packages of protobuf-c, gRPC, OpenSSL and json-c;
`clang-format` 18.1.8 for `just fmt`, which `ci/install-tools.sh` installs; and
[`just`](https://just.systems) 1.58.0 or newer. `just develop` also puts the published verification
tool, `yoke-verify`, on the `PATH`, which `just test` needs. The `Containerfile` builds a container
with all of it.

| Command | What it does |
| --- | --- |
| `just build` | Configures and builds, with CMake, the definitions, the base, the library and the programs that run their cases |
| `just test` | Runs the checks and the programs `just build` made, and checks each description against its tests |
| `just lint` | Parses every shell script |
| `just fmt` | Fails, naming each file, when a C file is not formatted |
| `just develop` | Verifies the installed `just` against a floor, given as `just develop 1.58.0`, and installs `yoke-verify` |
| `just release` | Hands the source archive to the release of the tag that names this commit; run by the release, not by hand |

## Where the depth is

The documentation at [docs.yoke-project.dev](https://docs.yoke-project.dev/): the
[reference](https://docs.yoke-project.dev/reference/) for the contract the library speaks, and the
[contributors' section](https://docs.yoke-project.dev/contributing/) for how the work is done.

## Licence

Apache 2.0, in [`LICENSE`](LICENSE), with a [`NOTICE`](NOTICE).
