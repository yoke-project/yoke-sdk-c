#!/usr/bin/env bash
# Installs, at a stated version, the tools this repository's verbs use beyond its language toolchain.
set -euo pipefail
python -m pip install --disable-pip-version-check clang-format==18.1.8
# The generators definitions/record names, as ubuntu-24.04 carries them, the well-known types' files
# protoc reads, protobuf-c's headers, and gRPC's core C API, which the library speaks through.
sudo apt-get install -y --no-install-recommends protobuf-compiler protobuf-c-compiler libprotobuf-dev libprotobuf-c-dev \
  libgrpc-dev pkg-config cmake
