#!/usr/bin/env bash
# The checks described by verbs-and-licence.std.md, one function per case.
# Each function prints why it failed and returns non-zero; checks/run.sh reports them.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
verbs=(build test lint fmt develop release)

# std: yoke-sdk-c:verbs-and-licence.01
check_verbs_defined() {
  [[ -f "$root/justfile" ]] || { echo "no justfile"; return 1; }
  local summary
  summary=" $(cd "$root" && just --summary) "
  for verb in "${verbs[@]}"; do
    [[ "$summary" == *" $verb "* ]] || { echo "verb '$verb' is not defined"; return 1; }
    local body
    body="$(cd "$root" && just --show "$verb" | grep -E '^[[:space:]]+[^[:space:]]')"
    [[ -n "$body" ]] || { echo "verb '$verb' has an empty body"; return 1; }
  done
}

# std: yoke-sdk-c:verbs-and-licence.02
check_no_path_outside() {
  [[ -f "$root/justfile" ]] || { echo "no justfile"; return 1; }
  local hits
  hits="$(cd "$root" && grep -nE '(^|[^.])\.\./' justfile go.mod go.work Cargo.toml pyproject.toml CMakeLists.txt package.json 2>/dev/null)"
  [[ -z "$hits" ]] || { echo "a path outside the repository: $hits"; return 1; }
}

# std: yoke-sdk-c:verbs-and-licence.03
check_container_build() {
  [[ -f "$root/Containerfile" ]] || { echo "no Containerfile"; return 1; }
  local engine
  engine="$(command -v podman || command -v docker)" || { echo "no container engine"; return 1; }
  "$engine" build -q -t yoke-sdk-c-check-build -f "$root/Containerfile" "$root" >/dev/null \
    || { echo "the image did not build"; return 1; }
  "$engine" run --rm yoke-sdk-c-check-build just build >/dev/null \
    || { echo "build failed inside the container"; return 1; }
}

# std: yoke-sdk-c:verbs-and-licence.04
check_licence() {
  [[ -f "$root/LICENSE" ]] || { echo "no LICENSE"; return 1; }
  grep -q 'Apache License' "$root/LICENSE" && grep -q 'Version 2.0, January 2004' "$root/LICENSE" \
    || { echo "LICENSE is not Apache 2.0"; return 1; }
  [[ -f "$root/NOTICE" ]] || { echo "no NOTICE"; return 1; }
  grep -q 'Davide Cardillo' "$root/NOTICE" || { echo "NOTICE does not name the copyright holder"; return 1; }
  local others
  others="$(cd "$root" && ls -d LICENSE.* LICENCE* COPYING* 2>/dev/null)"
  [[ -z "$others" ]] || { echo "a second licence file: $others"; return 1; }
}

# std: yoke-sdk-c:verbs-and-licence.05
check_develop_floor() {
  [[ -f "$root/justfile" ]] || { echo "no justfile"; return 1; }
  local out
  if out="$(cd "$root" && just develop 999.0.0 2>&1)"; then
    echo "develop passed with a floor above the installed just"; return 1
  fi
  [[ "$out" == *999.0.0* ]] || { echo "develop did not name the floor it was given"; return 1; }
  [[ "$out" == *"$(just --version | awk '{print $2}')"* ]] || { echo "develop did not name the version it found"; return 1; }
}

# std: yoke-sdk-c:verbs-and-licence.06
check_fmt_fails_on_unformatted() {
  [[ -f "$root/justfile" ]] || { echo "no justfile"; return 1; }
  local tmp out
  tmp="$(mktemp -d)"
  cp -a "$root/." "$tmp/"
  printf 'int  f( void ){return 0;}\n' > "$tmp/unformatted_check.c"
  if out="$(cd "$tmp" && just fmt 2>&1)"; then
    rm -rf "$tmp"; echo "fmt passed on an unformatted file"; return 1
  fi
  rm -rf "$tmp"
  [[ "$out" == *unformatted_check.c* ]] || { echo "fmt failed without naming the file"; return 1; }
}
