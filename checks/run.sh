#!/usr/bin/env bash
# Runs every check script under checks/, reporting each case by the identifier its marker names.
# Temporary: the verification tool's record writer (yoke-project/yoke#4) replaces the reporting.

dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
status=0
# A check script is sourced into this shell, so the names here are ones no check script sets.
for checks_file in "$dir"/*/*.sh; do
  # shellcheck source=/dev/null
  source "$checks_file"
  while read -r id fn; do
    # A marker the runner found no check after is a case that would otherwise reach the record absent.
    if [[ "$fn" == "-" ]]; then
      echo "FAIL  $id — no check follows its marker"
      status=1
      continue
    fi
    if out="$("$fn" 2>&1)"; then
      echo "pass  $id"
    else
      echo "FAIL  $id — $out"
      status=1
    fi
  done < <(awk '/# std: /{if (id) print id, "-"; id=$3; next}
               id && /^check_[a-z0-9_]+\(\)/{sub(/\(\).*/, ""); print id, $1; id=""}
               END{if (id) print id, "-"}' "$checks_file")
done
exit "$status"
