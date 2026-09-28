#!/usr/bin/env bash
# The checks described by the-published-tool.std.md, one function per case.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# A release served from a directory: for each version an archive holding a stand-in `yoke-verify`, and
# a manifest line naming it with its digest, or with the digest given after `=`.
# Usage: published_tool_release <dir> <version>[=<digest>]...
published_tool_release() {
  local dir="$1" architecture each version digest archive
  shift
  case "$(uname -m)" in
    x86_64) architecture=amd64 ;;
    aarch64 | arm64) architecture=arm64 ;;
    *) architecture="$(uname -m)" ;;
  esac
  mkdir -p "$dir/bin" "$dir/stage"
  : > "$dir/manifest.jsonl"
  for each in "$@"; do
    version="${each%%=*}"
    mkdir -p "$dir/v$version"
    printf '#!/bin/sh\necho "yoke-verify %s"\n' "$version" > "$dir/stage/yoke-verify"
    chmod +x "$dir/stage/yoke-verify"
    archive="yoke-verify-$version-linux-$architecture.tar.gz"
    tar -czf "$dir/v$version/$archive" -C "$dir/stage" yoke-verify
    digest="$(sha256sum "$dir/v$version/$archive" | awk '{print $1}')"
    [[ "$each" == *=* ]] && digest="${each#*=}"
    printf '{"line":"publication","published":"yoke-verify-linux-%s","version":"v%s","commit":"%s","digests":["sha256:%s"],"where":"https://example.invalid/","authenticated":"https://example.invalid","licence":"Apache-2.0","notices":["NOTICE"],"day":"2026-09-28"}\n' \
      "$architecture" "$version" "$(printf '0%.0s' {1..40})" "$digest" >> "$dir/manifest.jsonl"
  done
}

# `just develop` against the release served from dir, with dir/bin the user's bin directory at the head
# of PATH.
published_tool_develop() {
  local dir="$1"
  shift
  (cd "$root" && YOKE_MANIFEST="file://$dir/manifest.jsonl" YOKE_DOWNLOADS="file://$dir" \
    XDG_BIN_HOME="$dir/bin" PATH="$dir/bin:$PATH" just develop "$@" 2>&1)
}

# std: yoke-sdk-c:the-published-tool.01
check_develop_installs_the_version_given() {
  local dir out status=0
  dir="$(mktemp -d)"
  published_tool_release "$dir" 9.9.9
  if ! out="$(published_tool_develop "$dir" "" 9.9.9)"; then
    echo "develop failed: $out"; status=1
  elif [[ "$("$dir/bin/yoke-verify" 2>/dev/null)" != "yoke-verify 9.9.9" ]]; then
    echo "the user's bin directory holds no yoke-verify 9.9.9: $out"; status=1
  elif ! grep -q "yoke-verify 9.9.9 is on PATH .*the version given" <<<"$out"; then
    echo "develop does not name the version given: $out"; status=1
  fi
  rm -rf "$dir"
  return "$status"
}

# std: yoke-sdk-c:the-published-tool.02
check_develop_takes_the_newest_when_given_none() {
  local dir out status=0
  dir="$(mktemp -d)"
  published_tool_release "$dir" 9.9.10 9.9.8
  if ! out="$(published_tool_develop "$dir")"; then
    echo "develop failed: $out"; status=1
  elif [[ "$("$dir/bin/yoke-verify" 2>/dev/null)" != "yoke-verify 9.9.10" ]]; then
    echo "the newest was not installed: $out"; status=1
  elif ! grep -q "yoke-verify 9.9.10 is on PATH .*none was given" <<<"$out"; then
    echo "develop does not say that none was given: $out"; status=1
  fi
  rm -rf "$dir"
  return "$status"
}

# std: yoke-sdk-c:the-published-tool.03
check_develop_refuses_an_archive_its_line_does_not_name() {
  local dir out status=0
  dir="$(mktemp -d)"
  published_tool_release "$dir" "9.9.9=$(printf '0%.0s' {1..64})"
  if out="$(published_tool_develop "$dir" "" 9.9.9)"; then
    echo "develop accepted it: $out"; status=1
  elif [[ -e "$dir/bin/yoke-verify" ]]; then
    echo "a yoke-verify was installed"; status=1
  elif ! grep -q "not the file the manifest names" <<<"$out"; then
    echo "develop does not name the difference: $out"; status=1
  fi
  rm -rf "$dir"
  return "$status"
}

# std: yoke-sdk-c:the-published-tool.04
check_test_uses_the_tool_on_path() {
  local body
  body="$(cd "$root" && just --show test 2>/dev/null)" || { echo "no test verb"; return 1; }
  grep -qE 'yoke-verify descriptions .*--repository yoke-sdk-c' <<<"$body" || { echo "test does not check the descriptions"; return 1; }
  grep -qE 'yoke-verify markers .*--repository yoke-sdk-c' <<<"$body" || { echo "test does not check the markers"; return 1; }
  if grep -qE '[/@]yoke-verify|yoke-verify@|go run' <<<"$body"; then echo "test says where the tool comes from"; return 1; fi
  grep -qE 'command -v yoke-verify' <<<"$body" || { echo "test does not fail saying so when PATH lacks the tool"; return 1; }
}

# std: yoke-sdk-c:the-published-tool.05
check_integration_reaches_the_tool_through_develop() {
  local workflow="$root/.github/workflows/verify.yml" commands
  [[ -f "$workflow" ]] || { echo "no workflow"; return 1; }
  commands="$(grep -E '^[[:space:]]*(- )?run:' "$workflow" | sed -E 's/^[[:space:]]*(- )?run:[[:space:]]*//')"
  local develop test
  develop="$(grep -nx 'just develop' <<<"$commands" | head -n 1 | cut -d: -f1)"
  test="$(grep -nx 'just test' <<<"$commands" | head -n 1 | cut -d: -f1)"
  [[ -n "$develop" ]] || { echo "'just develop' is not run"; return 1; }
  [[ -n "$test" && "$develop" -lt "$test" ]] || { echo "'just develop' does not run before 'just test'"; return 1; }
  if grep -qE 'setup-go|go-version' "$workflow"; then echo "the workflow installs Go"; return 1; fi
}

# std: yoke-sdk-c:the-published-tool.06
check_the_record_names_the_tool_by_its_build_information() {
  local dir out status=0
  dir="$(mktemp -d)"
  mkdir -p "$dir/bin" "$dir/results"
  {
    printf '#!/usr/bin/env bash\n'
    printf '# mod\tgithub.com/yoke-project/yoke\tv9.9.9\t\n'
    printf '# build\tvcs.revision=0123456789abcdef0123456789abcdef01234567\n'
    printf 'printf "%%s\\n" "$@" > "%s/arguments"\n' "$dir"
  } > "$dir/bin/yoke-verify"
  chmod +x "$dir/bin/yoke-verify"
  echo "pass  yoke-sdk-c:the-published-tool.06" > "$dir/results/checks.txt"
  echo 2026-09-28T00:00:00Z > "$dir/results/started"
  echo 2026-09-28T00:00:01Z > "$dir/results/finished"
  if ! out="$(PATH="$dir/bin:$PATH" bash "$root/ci/record.sh" "$dir/results" 2>&1)"; then
    echo "record.sh failed: $out"; status=1
  elif ! grep -qx 'yoke-verify=v9.9.9@0123456789ab' "$dir/arguments" 2>/dev/null; then
    echo "the tool was not named by its build information: $(cat "$dir/arguments" 2>/dev/null | tr '\n' ' ')"; status=1
  fi
  rm -rf "$dir"
  return "$status"
}
