#!/usr/bin/env bash
# The checks described by the-definitions.std.md, one function per case.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# The value a field of the record carries.
definitions_field() {
  awk -v key="$1" '$1 == key { $1 = ""; sub(/^ /, ""); print }' "$root/definitions/record"
}

# A proxy serving one module zip, holding one file, and a manifest whose line for it carries the digest
# given. Usage: definitions_served <dir> <digest>
definitions_served() {
  local dir="$1" digest="$2" module=github.com/yoke-project/yoke/proto version=v9.9.9
  mkdir -p "$dir/stage/$module@$version/yoke/plugin/v1" "$dir/proxy/$module/@v"
  printf 'syntax = "proto3";\npackage yoke.plugin.v1;\n' > "$dir/stage/$module@$version/yoke/plugin/v1/one.proto"
  (cd "$dir/stage" && zip -qr "$dir/proxy/$module/@v/$version.zip" .)
  printf '{"line":"publication","published":"%s","version":"%s","digests":["%s"]}\n' "$module" "$version" "$digest" > "$dir/manifest.jsonl"
  printf 'module %s\nversion %s\ndigest %s\nprotoc %s\nprotoc-c %s\ncontracts plugin\n' "$module" "$version" "$digest" \
    "$(definitions_field protoc)" "$(definitions_field protoc-c)" > "$dir/record"
}

# std: yoke-sdk-c:the-definitions.01
check_the_record_names_what_the_manifest_publishes() {
  [[ -f "$root/definitions/record" ]] || { echo "there is no definitions/record"; return 1; }
  local module version digest field lines
  module="$(definitions_field module)" version="$(definitions_field version)" digest="$(definitions_field digest)"
  [[ "$module" == github.com/yoke-project/yoke/proto ]] || { echo "the record names the module '$module'"; return 1; }
  [[ "$version" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "the record names the version '$version'"; return 1; }
  [[ "$digest" == h1:* ]] || { echo "the record names the digest '$digest'"; return 1; }
  for field in protoc protoc-c contracts; do
    [[ -n "$(definitions_field "$field")" ]] || { echo "the record names no $field"; return 1; }
  done
  lines="$(curl -fsSL "${YOKE_MANIFEST:-https://raw.githubusercontent.com/yoke-project/yoke/main/releases/manifest.jsonl}")" ||
    { echo "the release manifest cannot be read"; return 1; }
  grep -F "\"published\":\"$module\"" <<<"$lines" | grep -F "\"version\":\"$version\"" | grep -qF "\"$digest\"" ||
    { echo "the manifest publishes no $module $version with the digest $digest"; return 1; }
}

# std: yoke-sdk-c:the-definitions.02
check_a_module_not_published_is_refused() {
  local dir out
  dir="$(mktemp -d)"
  definitions_served "$dir" "h1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
  mkdir "$dir/out"
  if out="$(YOKE_MANIFEST="file://$dir/manifest.jsonl" YOKE_PROXY="file://$dir/proxy" DEFINITIONS_RECORD="$dir/record" \
    bash "$root/ci/definitions.sh" generate "$dir/out" 2>&1)"; then
    rm -rf "$dir"
    echo "a module with another digest was accepted: $out"
    return 1
  fi
  if [[ "$out" != *"h1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="* || "$out" != *"computed h1:"* || -n "$(ls -A "$dir/out")" ]]; then
    rm -rf "$dir"
    echo "the refusal said: $out"
    return 1
  fi
  rm -rf "$dir"
}

# std: yoke-sdk-c:the-definitions.03
check_another_generator_is_refused() {
  local dir out
  dir="$(mktemp -d)"
  printf '#!/bin/sh\necho "libprotoc 9.9.9"\n' > "$dir/protoc"
  chmod +x "$dir/protoc"
  if out="$(PATH="$dir:$PATH" bash "$root/ci/definitions.sh" check 2>&1)"; then
    rm -rf "$dir"
    echo "another protoc was accepted: $out"
    return 1
  fi
  rm -rf "$dir"
  [[ "$out" == *9.9.9* && "$out" == *"$(definitions_field protoc)"* ]] || { echo "the refusal said: $out"; return 1; }
}

# std: yoke-sdk-c:the-definitions.04
check_the_committed_tree_is_what_is_generated() {
  local out
  out="$(bash "$root/ci/definitions.sh" check 2>&1)" || { echo "$out"; return 1; }
}

# std: yoke-sdk-c:the-definitions.05
check_a_contract_the_generator_cannot_express_is_refused() {
  local dir out
  dir="$(mktemp -d)"
  sed 's/^contracts .*/contracts administrative/' "$root/definitions/record" > "$dir/record"
  mkdir "$dir/out"
  if out="$(DEFINITIONS_RECORD="$dir/record" bash "$root/ci/definitions.sh" generate "$dir/out" 2>&1)"; then
    rm -rf "$dir"
    echo "the administrative contract was generated: $out"
    return 1
  fi
  if [[ "$out" != *"contract administrative"* || -d "$dir/out/yoke/administrative" ]]; then
    rm -rf "$dir"
    echo "the refusal said: $out"
    return 1
  fi
  rm -rf "$dir"
}

# std: yoke-sdk-c:the-definitions.06
check_the_generated_tree_compiles() {
  local out
  out="$(cd "$root" && just build 2>&1)" || { echo "$out"; return 1; }
}
