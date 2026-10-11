# yoke-sdk-c

## What it is

The C family of Yoke's SDKs: the library a C program uses to run as a Plugin unit in a Yoke
instance, over a base it shares with the libraries to come, and the harness that holds it to the
contract under the conformance suite. It speaks gRPC through gRPC's core C API, and its messages are
generated with protobuf-c.

It is not the Core, and it holds no contract definitions of its own: the C it builds on is generated
from the published definitions, at the version `definitions/record` pins. It is distributed as
source; nothing is published from it to a package registry yet.

## What is where

| Path | What it holds |
| --- | --- |
| `include/yoke/` | The public headers: `base.h` and `plugin.h` |
| `base/` | What the libraries share: the connection, the envelope, a refusal |
| `plugin/` | The library a Plugin unit is written with |
| `harness/` | The library's harness for the conformance suite |
| `definitions/` | The generated C of the plugin and interface contracts, and `record`, which pins the definitions' version, digest and generators |
| `testing/` | What the programs that run the cases share |
| `checks/`, `ci/` | This repository's own checks, and the scripts continuous integration runs; `ci/definitions.sh` regenerates `definitions/` |

A test's description sits beside the test, as a `.std.md` file.

## Working on it

A C compiler, CMake and `pkg-config`; the development packages of protobuf-c, gRPC and json-c;
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
| `just release` | Says there is nothing to publish yet |

## Where the depth is

The documentation at [docs.yoke-project.dev](https://docs.yoke-project.dev/): the
[reference](https://docs.yoke-project.dev/reference/) for the contract the library speaks, and the
[contributors' section](https://docs.yoke-project.dev/contributing/) for how the work is done.

## Licence

Apache 2.0, in [`LICENSE`](LICENSE), with a [`NOTICE`](NOTICE).
