#!/usr/bin/env bash
# Runs every check script under checks/, reporting each case by the identifier its marker names.
# Temporary: the verification tool's record writer (yoke-project/yoke#4) replaces the reporting.

dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
status=0
for script in "$dir"/*/*.sh; do
  # shellcheck source=/dev/null
  source "$script"
  while read -r id fn; do
    if out="$("$fn" 2>&1)"; then
      echo "pass  $id"
    else
      echo "FAIL  $id — $out"
      status=1
    fi
  done < <(awk '/# std: /{id=$3; next} id && /^check_[a-z_]+\(\)/{sub(/\(\).*/, ""); print id, $1; id=""}' "$script")
done
exit "$status"
