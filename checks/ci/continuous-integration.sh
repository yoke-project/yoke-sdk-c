#!/usr/bin/env bash
# The checks described by continuous-integration.std.md, one function per case.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
workflow="$root/.github/workflows/verify.yml"

# The command of every `run:` step, one per line.
run_commands() {
  grep -E '^[[:space:]]*(- )?run:' "$workflow" | sed -E 's/^[[:space:]]*(- )?run:[[:space:]]*//'
}

# std: yoke-sdk-c:continuous-integration.01
check_verbs_on_every_change() {
  [[ -f "$workflow" ]] || { echo "no workflow"; return 1; }
  grep -qE '^[[:space:]]*pull_request:' "$workflow" || { echo "the workflow does not run on a proposed change"; return 1; }
  local commands verb
  commands="$(run_commands)"
  for verb in build test lint fmt; do
    grep -qx "just $verb" <<<"$commands" || { echo "'just $verb' is not run"; return 1; }
  done
  local other
  other="$(grep -vE '^just (build|test|lint|fmt)$' <<<"$commands" | grep -vE '^ci/' || true)"
  [[ -z "$other" ]] || { echo "a step does work of its own: $other"; return 1; }
}

# std: yoke-sdk-c:continuous-integration.02
check_toolchain_declared() {
  [[ -f "$workflow" ]] || { echo "no workflow"; return 1; }
  local runners
  runners="$(grep -E '^[[:space:]]*runs-on:' "$workflow" | sed -E 's/^[[:space:]]*runs-on:[[:space:]]*//')"
  [[ -n "$runners" ]] || { echo "no job names its runner"; return 1; }
  if grep -qvE '^ubuntu-[0-9]+\.[0-9]+$' <<<"$runners"; then echo "a runner image is not pinned: $runners"; return 1; fi
  grep -qE 'just-version:[[:space:]]*[0-9]' "$workflow" || { echo "the runner's version is not stated"; return 1; }
  grep -qE 'python-version:[[:space:]]*.?[0-9]' "$workflow" || { echo "the language toolchain's version is not stated"; return 1; }
}
