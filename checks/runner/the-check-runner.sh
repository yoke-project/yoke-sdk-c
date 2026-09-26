#!/usr/bin/env bash
# The checks described by the-check-runner.std.md, one function per case.
# Each runs a copy of the runner on a tree of its own. That tree's markers are written when the check
# runs, never spelled in this file, so no scan of this repository takes them for its own.

# runner_tree writes a copy of the runner beside one check script, whose lines are given, and prints
# where. It is sourced into the runner's own shell, so it keeps every name it uses local.
runner_tree() {
  local tree
  tree="$(mktemp -d)"
  mkdir -p "$tree/checks/fake"
  cp "$(dirname "${BASH_SOURCE[0]}")/../run.sh" "$tree/checks/run.sh"
  local line
  for line in "$@"; do
    if [[ "$line" == "MARK "* ]]; then printf '# %s: %s\n' std "${line#MARK }"; else printf '%s\n' "$line"; fi
  done > "$tree/checks/fake/fake.sh"
  echo "$tree"
}

# std: yoke-sdk-c:the-check-runner.01
check_a_check_named_with_a_digit_is_run() {
  local tree said
  tree="$(runner_tree 'MARK fake:runner.01' 'check_l3_holds() { true; }')"
  said="$(bash "$tree/checks/run.sh" 2>&1)"
  rm -rf "$tree"
  grep -qx 'pass  fake:runner.01' <<<"$said" || { echo "the runner said: ${said:-nothing}"; return 1; }
}

# std: yoke-sdk-c:the-check-runner.02
check_a_marker_whose_check_cannot_be_found_is_reported() {
  local tree said code
  tree="$(runner_tree 'MARK fake:runner.02' 'check_Missing() { true; }' 'MARK fake:runner.03')"
  said="$(bash "$tree/checks/run.sh" 2>&1)"
  code=$?
  rm -rf "$tree"
  local each
  for each in fake:runner.02 fake:runner.03; do
    grep -q "^FAIL  $each — no check follows its marker" <<<"$said" \
      || { echo "for $each the runner said: ${said:-nothing}"; return 1; }
  done
  (( code != 0 )) || { echo "the runner exited zero"; return 1; }
}
