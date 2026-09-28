#!/usr/bin/env bash
# Assembles the record of a run from what that run left behind, and writes it to standard output.
# It runs nothing: a record is evidence of a run that already happened.
#
# The tool is the one `develop` put on PATH, and the record names it by the build information the
# binary carries: the version it states, and nothing learnt from where it was installed.
# Usage: record.sh [results directory]
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
results="${1:-$root/.results}"

for each in checks.txt started finished; do
  [[ -f "$results/$each" ]] || { echo "record: the run left no $each in $results" >&2; exit 1; }
done

# The architecture as the environment's dimension names it.
case "$(uname -m)" in
  x86_64) architecture=amd64 ;;
  aarch64 | arm64) architecture=arm64 ;;
  *) architecture="$(uname -m)" ;;
esac

tool="$(command -v yoke-verify)" || { echo "record: yoke-verify is not on PATH; \`just develop\` puts it there" >&2; exit 1; }
version="$(grep -aoE $'mod\tgithub\\.com/yoke-project/yoke\t[^\t]+' "$tool" | head -n 1 | cut -f3)"
revision="$(grep -aoE 'vcs\.revision=[0-9a-f]{40}' "$tool" | head -n 1 | cut -d= -f2)"

yoke-verify record \
  --level L1 \
  --tier reference \
  --repository yoke-sdk-c \
  --environment "architecture=$architecture" \
  --started "$(tr -d '[:space:]' < "$results/started")" \
  --finished "$(tr -d '[:space:]' < "$results/finished")" \
  --ran "yoke-verify=${version:-unknown}@${revision:0:12}" \
  --results "$results/checks.txt" \
  "$root"
