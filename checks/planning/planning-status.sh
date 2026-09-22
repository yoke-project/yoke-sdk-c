#!/usr/bin/env bash
# The checks described by planning-status.std.md, one function per case.
# Nothing here reaches the network: the script under check is sourced, and its pure parts are asked
# directly what they would do.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
planning_workflow="$root/.github/workflows/planning.yml"
planning_script="$root/ci/planning-status.sh"

# std: yoke-sdk-c:planning-status.01
check_workflow_runs_the_script_alone() {
  [[ -f "$planning_workflow" ]] || { echo "no planning workflow"; return 1; }
  grep -qE '^[[:space:]]*pull_request:' "$planning_workflow" || { echo "it does not start on a proposed change"; return 1; }
  grep -qE '^[[:space:]]*push:' "$planning_workflow" || { echo "it does not start on a push"; return 1; }
  grep -qE '^[[:space:]]*branches-ignore:.*main' "$planning_workflow" \
    || { echo "a push to the default branch starts it too"; return 1; }

  local commands other
  commands="$(grep -E '^[[:space:]]*(- )?run:' "$planning_workflow" | sed -E 's/^[[:space:]]*(- )?run:[[:space:]]*//')"
  [[ -n "$commands" ]] || { echo "it runs nothing"; return 1; }
  other="$(grep -vE '^ci/' <<<"$commands")"
  [[ -z "$other" ]] || { echo "a command that is not a script under ci/: $other"; return 1; }
  grep -qE '^ci/planning-status\.sh .*github\.event_name' <<<"$commands" \
    || { echo "the script is not given the event that started the workflow"; return 1; }
}

# std: yoke-sdk-c:planning-status.02
check_granted_nothing_and_silent_without_a_credential() {
  [[ -f "$planning_workflow" && -x "$planning_script" ]] || { echo "no workflow or no script"; return 1; }
  grep -qE '^[[:space:]]*permissions:' "$planning_workflow" || { echo "the job is granted whatever the default is"; return 1; }
  local granted
  granted="$(sed -n '/^[[:space:]]*permissions:/,/^[[:space:]]*[a-z-]*:[[:space:]]*$/p' "$planning_workflow" \
    | grep -E '^[[:space:]]+[a-z-]+:' | grep -vE '^[[:space:]]*permissions:')"
  local line
  while read -r line; do
    [[ -z "$line" ]] && continue
    [[ "$line" =~ ^[[:space:]]*contents:[[:space:]]*read$ ]] \
      || { echo "the job is granted more than reading its own tree: $line"; return 1; }
  done <<<"$granted"

  local out status
  out="$(GH_TOKEN="" GITHUB_REPOSITORY=yoke-project/yoke-sdk-c "$planning_script" push 2>&1)"
  status=$?
  (( status == 0 )) || { echo "with no credential it exited $status: $out"; return 1; }
  [[ "$out" == *credential* ]] || { echo "with no credential it did not say so: $out"; return 1; }
}

# std: yoke-sdk-c:planning-status.03
check_an_item_moves_forward_only() {
  [[ -f "$planning_script" ]] || { echo "no script"; return 1; }
  # shellcheck source=/dev/null
  source "$planning_script"

  local from target got
  for from in "" "Todo" "In Progress" "In Review" "Done"; do
    for target in "In Progress" "In Review"; do
      got="$(forward "$from" "$target")"
      case "$from|$target" in
        "|In Progress"|"|In Review"|"Todo|In Progress"|"Todo|In Review"|"In Progress|In Review")
          [[ "$got" == "$target" ]] || { echo "${from:-no state} does not move to $target"; return 1; } ;;
        *)
          [[ -z "$got" ]] || { echo "${from:-no state} moves to $got, and that is not forward"; return 1; } ;;
      esac
    done
  done

  got="$(forward "In Review" "Done")"
  [[ -z "$got" ]] || { echo "it moves an item to Done, which is the planning tool's"; return 1; }
}

# std: yoke-sdk-c:planning-status.04
check_items_are_read_from_the_change() {
  [[ -f "$planning_script" ]] || { echo "no script"; return 1; }
  command -v jq >/dev/null || { echo "no jq"; return 1; }
  # shellcheck source=/dev/null
  source "$planning_script"

  local tmp event named
  tmp="$(mktemp -d)"
  event="$tmp/event.json"
  cat > "$event" <<'JSON'
{
  "commits": [
    { "message": "Describe the thing\n\nCloses #3." },
    { "message": "Follow it in the layer\n\nPart of yoke-project/meta-yoke#7" },
    { "message": "Raise the floor to 1.58.0, and nothing else" },
    { "message": "A commit naming no item at all" }
  ]
}
JSON
  named="$(items_named_by_push "$event" "yoke-project/yoke-sdk-c")"
  rm -rf "$tmp"

  local want
  want="$(printf '%s
' "yoke-project/meta-yoke#7" "yoke-project/yoke-sdk-c#3" | sort)"
  [[ "$(sort <<<"$named")" == "$want" ]] || { echo "it read: $(tr '\n' ' ' <<<"$named")"; return 1; }
}
