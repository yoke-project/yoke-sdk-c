#!/usr/bin/env bash
# Installs, at a stated version, the tools this repository's verbs use beyond its language toolchain.
set -euo pipefail
python -m pip install --disable-pip-version-check clang-format==18.1.8
