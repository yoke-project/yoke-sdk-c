#!/usr/bin/env bash
# Puts the published verification tool on PATH, in the user's bin directory, at the version given or,
# with none, the newest the release manifest names. The archive is authenticated by the digest on its
# line in the manifest; on a difference nothing is installed.
# Usage: yoke-verify.sh [version]
set -euo pipefail

manifest="${YOKE_MANIFEST:-https://raw.githubusercontent.com/yoke-project/yoke/main/releases/manifest.jsonl}"
downloads="${YOKE_DOWNLOADS:-https://github.com/yoke-project/yoke/releases/download}"
bin="${XDG_BIN_HOME:-$HOME/.local/bin}"
version="${1:-}"

[[ "$(uname -s)" == Linux ]] || { echo "develop: yoke-verify is published for Linux only"; exit 1; }
case "$(uname -m)" in
  x86_64) architecture=amd64 ;;
  aarch64 | arm64) architecture=arm64 ;;
  *) echo "develop: no yoke-verify is published for $(uname -m)"; exit 1 ;;
esac

lines="$(curl -fsSL "$manifest")" || { echo "develop: the release manifest cannot be read from $manifest"; exit 1; }
published="\"published\":\"yoke-verify-linux-$architecture\""
if [[ -z "$version" ]]; then
  version="$(grep -F "$published" <<<"$lines" | sed -E 's/.*"version":"v([^"]+)".*/\1/' | sort -V | tail -n 1)"
  [[ -n "$version" ]] || { echo "develop: the manifest names no yoke-verify for linux-$architecture"; exit 1; }
  taken="none was given, so the newest the manifest names"
else
  [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "develop: '$version' is not a version of yoke-verify"; exit 1; }
  taken="the version given"
fi
line="$(grep -F "$published" <<<"$lines" | grep -F "\"version\":\"v$version\"" | head -n 1 || true)"
[[ -n "$line" ]] || { echo "develop: the manifest names no yoke-verify $version for linux-$architecture"; exit 1; }
digest="$(sed -E 's/.*"digests":\["sha256:([0-9a-f]{64})".*/\1/' <<<"$line")"
[[ "$digest" =~ ^[0-9a-f]{64}$ ]] || { echo "develop: the manifest's line for yoke-verify $version names no sha256"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
archive="yoke-verify-$version-linux-$architecture.tar.gz"
curl -fsSL -o "$work/$archive" "$downloads/v$version/$archive" || { echo "develop: $archive cannot be downloaded"; exit 1; }
got="$(sha256sum "$work/$archive" | awk '{print $1}')"
if [[ "$got" != "$digest" ]]; then
  echo "develop: $archive is not the file the manifest names (sha256 $got, the manifest says $digest); nothing installed"
  exit 1
fi
tar -xzf "$work/$archive" -C "$work" yoke-verify
mkdir -p "$bin"
install -m 0755 "$work/yoke-verify" "$bin/yoke-verify"

found="$(command -v yoke-verify || true)"
if [[ -z "$found" ]]; then
  echo "develop: yoke-verify $version is in $bin, which PATH does not reach"
  exit 1
fi
if [[ ! "$found" -ef "$bin/yoke-verify" ]]; then
  echo "develop: yoke-verify $version is in $bin, but PATH finds $found first"
  exit 1
fi
echo "develop: yoke-verify $version is on PATH at $bin/yoke-verify — $taken"
