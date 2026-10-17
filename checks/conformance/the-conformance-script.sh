#!/usr/bin/env bash
# The conformance script, run with the suite a caller hands it: the release of `yoke` runs it this way,
# at the commit this family's newest publication names, with the suite of its own commit. The build tools
# and the suite are stand-ins on PATH, so what is checked is the script and not the build.

conformance_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
conformance_harnesses=(yoke-c-plugin-harness)

# conformance_stand_ins <dir>: a suite that writes down how it was called and writes any report it is
# asked for, a Core that is never run, and build tools that build nothing.
conformance_stand_ins() {
  mkdir -p "$1/suite" "$1/tools" "$1/reports"
  cat > "$1/suite/yoke-conformance" <<'EOF'
#!/usr/bin/env bash
echo "$*" >> "$(dirname "$0")/calls"
while (( $# )); do [[ "$1" == --report ]] && echo '{}' > "$2"; shift; done
EOF
  printf '#!/bin/sh\nexit 1\n' > "$1/suite/yoke-core"
  local tool
  for tool in cmake cargo go; do printf '#!/bin/sh\nexit 0\n' > "$1/tools/$tool"; done
  printf '#!/bin/sh\necho %s/venv\n' "$1" > "$1/tools/just"
  chmod +x "$1"/suite/* "$1"/tools/*
}

# std: yoke-sdk-c:the-conformance-script.01
check_a_suite_handed_over_is_the_one_run() {
  local tmp out why="" h
  tmp="$(mktemp -d)"
  conformance_stand_ins "$tmp"
  out="$(PATH="$tmp/tools:$PATH" CONFORMANCE_SUITE="$tmp/suite" YOKE_MANIFEST=http://127.0.0.1:9/none \
    YOKE_DOWNLOADS=http://127.0.0.1:9/none GOPROXY=off bash "$conformance_root/ci/conformance.sh" "" "$tmp/results" 2>&1)" \
    || why+=" the script failed: $(tail -2 <<<"$out" | tr '\n' ' ');"
  for h in "${conformance_harnesses[@]}"; do
    grep -q -- "--core $tmp/suite/yoke-core .*--harness [^ ]*/$h\b" "$tmp/suite/calls" 2> /dev/null \
      || why+=" the suite handed over was not run against $h with its Core;"
  done
  rm -rf "$tmp"
  [[ -z "$why" ]] || { echo "${why# }"; return 1; }
}

# std: yoke-sdk-c:the-conformance-script.02
check_each_harness_writes_its_report_where_asked() {
  local tmp out why="" h
  tmp="$(mktemp -d)"
  conformance_stand_ins "$tmp"
  out="$(PATH="$tmp/tools:$PATH" CONFORMANCE_SUITE="$tmp/suite" CONFORMANCE_REPORTS="$tmp/reports" \
    YOKE_MANIFEST=http://127.0.0.1:9/none YOKE_DOWNLOADS=http://127.0.0.1:9/none GOPROXY=off \
    bash "$conformance_root/ci/conformance.sh" "" "$tmp/results" 2>&1)" || why+=" the script failed: $(tail -2 <<<"$out" | tr '\n' ' ');"
  for h in "${conformance_harnesses[@]}"; do
    [[ -f "$tmp/reports/$h.json" ]] || why+=" no report $h.json;"
  done
  rm -rf "$tmp"
  [[ -z "$why" ]] || { echo "${why# }"; return 1; }
}
