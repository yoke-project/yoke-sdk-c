#!/usr/bin/env bash
# The checks described by the-record-of-a-run.std.md, one function per case.
# The two that read a record build their results as a fixture, so the record of a failing run is
# checked without having to fail one.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
record_workflow="$root/.github/workflows/verify.yml"
record_script="$root/ci/record.sh"

# std: yoke-sdk-c:the-record-of-a-run.01
# It reads the verb and never the results: this check runs inside the run it would be inspecting.
check_a_run_leaves_its_results() {
  local recipe
  recipe="$(cd "$root" && just --show test 2>/dev/null)"
  [[ -n "$recipe" ]] || { echo "no test verb"; return 1; }

  local each
  for each in 'started' 'finished' 'checks\.txt'; do
    grep -qE "\.results/$each" <<<"$recipe" || { echo "the verb does not write .results/${each//\\/}"; return 1; }
  done
  grep -qE 'tee \.results/checks\.txt' <<<"$recipe" \
    || { echo "the checks' lines are not kept as they are written"; return 1; }
  grep -qE 'date -u .*started' <<<"$recipe" && grep -qE 'date -u .*finished' <<<"$recipe" \
    || { echo "the run does not record when it began and ended"; return 1; }

  grep -qE '\|\| status=1' <<<"$recipe" || { echo "a failure abandons the rest of the run"; return 1; }
  grep -qE 'exit "\$status"' <<<"$recipe" || { echo "the verb does not carry the failure to its own exit"; return 1; }
  grep -qE '^[[:space:]]*set -euo' <<<"$recipe" && { echo "the verb aborts on the first failure, and leaves the rest unwritten"; return 1; }
  return 0
}

# A directory of results, with the verdict given for one case of this repository's own descriptions.
record_fixture() {
  local directory="$1" verdict="$2" id="$3"
  mkdir -p "$directory"
  if [[ "$verdict" == pass ]]; then
    printf 'pass  %s\n' "$id" > "$directory/checks.txt"
  else
    printf 'FAIL  %s — the fixture says so\n' "$id" > "$directory/checks.txt"
  fi
  date -u +%Y-%m-%dT%H:%M:%SZ > "$directory/started"
  date -u +%Y-%m-%dT%H:%M:%SZ > "$directory/finished"
}

# std: yoke-sdk-c:the-record-of-a-run.02
check_the_record_is_assembled_from_them() {
  [[ -x "$record_script" ]] || { echo "no ci/record.sh"; return 1; }
  command -v python3 >/dev/null || { echo "no python3"; return 1; }
  command -v go >/dev/null || { echo "no go, and the tool comes from the module proxy (472)"; return 1; }

  local tmp written
  tmp="$(mktemp -d)"
  record_fixture "$tmp" pass "yoke-sdk-c:verbs-and-licence.01"
  # Only what it wrote: the tool the proxy fetches says so on the error stream, and a record is JSON.
  if ! written="$("$record_script" "$tmp" 2>"$tmp/said")"; then
    echo "the script refused the results: $(cat "$tmp/said")"; rm -rf "$tmp"; return 1
  fi
  rm -rf "$tmp"

  python3 - "$written" <<'PY' || return 1
import json, sys
record = json.loads(sys.argv[1])
for field in ("schema", "repository", "commit", "level", "tier", "environment",
              "started", "finished", "ran", "state", "blocks", "cases"):
    if field not in record:
        raise SystemExit(f"the record carries no {field}")
if record["repository"] != "yoke-sdk-c":
    raise SystemExit(f'the record names the repository {record["repository"]!r}')
if not record["commit"]:
    raise SystemExit("the record names no commit")
if record["level"] != "L1" or record["tier"] != "reference":
    raise SystemExit(f'the record is of {record["level"]} {record["tier"]}')
if not record["environment"] or not record["ran"]:
    raise SystemExit("the record names no environment or no tool")
if not record["cases"]:
    raise SystemExit("the record holds no case")
PY
}

# std: yoke-sdk-c:the-record-of-a-run.03
check_a_failing_run_is_recorded_as_one() {
  [[ -x "$record_script" ]] || { echo "no ci/record.sh"; return 1; }
  command -v go >/dev/null || { echo "no go, and the tool comes from the module proxy (472)"; return 1; }

  local tmp written
  tmp="$(mktemp -d)"
  record_fixture "$tmp" fail "yoke-sdk-c:verbs-and-licence.01"
  written="$("$record_script" "$tmp" 2>"$tmp/said")"     || { echo "the script refused the results: $(cat "$tmp/said")"; rm -rf "$tmp"; return 1; }
  rm -rf "$tmp"

  python3 - "$written" <<'PY' || return 1
import json, sys
record = json.loads(sys.argv[1])
if record["state"] != "failed":
    raise SystemExit(f'the state is {record["state"]!r}, and a case failed')
if not record["blocks"]:
    raise SystemExit("blocks is false, and the case that failed is blocking")
if not [c for c in record["cases"] if c["result"] == "fail"]:
    raise SystemExit("no case is recorded as failing")
PY
}

# std: yoke-sdk-c:the-record-of-a-run.04
check_the_workflow_hands_it_over() {
  [[ -f "$record_workflow" ]] || { echo "no workflow"; return 1; }
  grep -qE '^[[:space:]]*- run: ci/record\.sh' "$record_workflow" \
    || { echo "no step assembles the record with a script under ci/"; return 1; }
  grep -qE 'upload-artifact' "$record_workflow" || { echo "the record is not handed over"; return 1; }
  local always
  always="$(grep -cE '^[[:space:]]*if: always\(\)' "$record_workflow")"
  (( always >= 2 )) || { echo "the record's steps do not run whatever the verbs decided"; return 1; }
}

# std: yoke-sdk-c:the-record-of-a-run.05
check_nothing_a_run_writes_enters_the_tree() {
  [[ -f "$root/.gitignore" ]] || { echo "no .gitignore"; return 1; }
  grep -qE '^\.results' "$root/.gitignore" || { echo ".gitignore does not ignore the results"; return 1; }
  # A path under it, not the directory itself: a directory rule matches nothing when nothing is there.
  (cd "$root" && git check-ignore -q .results/checks.txt) || { echo "the results are not ignored"; return 1; }
  local seen
  seen="$(cd "$root" && git status --porcelain | grep -F '.results' || true)"
  [[ -z "$seen" ]] || { echo "a run left the results in the tree: $seen"; return 1; }
}
