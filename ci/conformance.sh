#!/usr/bin/env bash
# The conformance suite against this family's plugin harness: L2, which blocks.
#
# The suite and the Core it drives are yoke's published pair of binaries, downloaded, authenticated by
# the digest on their line in yoke's release manifest, and never built: a contributor needs no Go. The
# harness is built from this checkout. With no version, the newest the manifest names.
# Usage: conformance.sh [version] [results directory]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="${1:-}"
results="${2:-$root/.results}"
manifest="${YOKE_MANIFEST:-https://raw.githubusercontent.com/yoke-project/yoke/main/releases/manifest.jsonl}"
downloads="${YOKE_DOWNLOADS:-https://github.com/yoke-project/yoke/releases/download}"

case "$(uname -m)" in
  x86_64) architecture=amd64 ;;
  aarch64 | arm64) architecture=arm64 ;;
  *) echo "conformance: no suite is published for $(uname -m)"; exit 1 ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
lines="$(curl -fsSL "$manifest")"
published="\"published\":\"yoke-conformance-linux-$architecture\""
if [[ -z "$version" ]]; then
  version="$(grep -F "$published" <<<"$lines" | sed -E 's/.*"version":"v([^"]+)".*/\1/' | sort -V | tail -n 1)"
fi
line="$(grep -F "$published" <<<"$lines" | grep -F "\"version\":\"v$version\"" | head -n 1 || true)"
[[ -n "$line" ]] || { echo "conformance: the manifest names no suite $version for linux-$architecture"; exit 1; }
digest="$(sed -E 's/.*"digests":\["sha256:([0-9a-f]{64})".*/\1/' <<<"$line")"
archive="yoke-conformance-$version-linux-$architecture.tar.gz"
curl -fsSL -o "$work/$archive" "$downloads/v$version/$archive"
got="$(sha256sum "$work/$archive" | awk '{print $1}')"
[[ "$got" == "$digest" ]] || { echo "conformance: $archive is not the file the manifest names (sha256 $got, the manifest says $digest)"; exit 1; }
tar -xzf "$work/$archive" -C "$work" yoke-conformance yoke-core
echo "conformance: yoke $version, authenticated"

cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
cmake --build "$root/build" --target yoke-c-plugin-harness --parallel > /dev/null
mkdir -p "$results"
"$work/yoke-conformance" --core "$work/yoke-core" --harness "$root/build/yoke-c-plugin-harness" | tee "$results/conformance.txt"
