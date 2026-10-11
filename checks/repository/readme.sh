#!/usr/bin/env bash
# The checks described by readme.std.md, one function per case.
# Each function prints why it failed and returns non-zero; checks/run.sh reports them.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readme_headings=("What it is" "What is where" "Working on it" "Where the depth is" "Licence")
readme_verbs=(build test lint fmt develop release)

# std: yoke-sdk-c:readme.01
check_readme_headings() {
  [[ -f "$root/README.md" ]] || { echo "no README.md"; return 1; }
  local found expected
  found="$(grep -E '^## ' "$root/README.md" | sed 's/^## //' | grep -Fx -f <(printf '%s\n' "${readme_headings[@]}"))"
  expected="$(printf '%s\n' "${readme_headings[@]}")"
  [[ "$found" == "$expected" ]] || { echo "the headings are not the five in order; found: $(echo $found)"; return 1; }
}

# std: yoke-sdk-c:readme.02
check_readme_verbs() {
  [[ -f "$root/README.md" ]] || { echo "no README.md"; return 1; }
  local section
  section="$(awk '/^## /{on = ($0 == "## Working on it")} on' "$root/README.md")"
  for verb in "${readme_verbs[@]}"; do
    [[ "$section" == *"just $verb"* ]] || { echo "Working on it does not name 'just $verb'"; return 1; }
  done
}

# std: yoke-sdk-c:readme.03
check_readme_docs_link() {
  [[ -f "$root/README.md" ]] || { echo "no README.md"; return 1; }
  local section
  section="$(awk '/^## /{on = ($0 == "## Where the depth is")} on' "$root/README.md")"
  [[ "$section" == *"https://docs.yoke-project.dev"* ]] || { echo "Where the depth is does not link the documentation"; return 1; }
}

# std: yoke-sdk-c:readme.04
check_readme_names_nothing_private() {
  [[ -f "$root/README.md" ]] || { echo "no README.md"; return 1; }
  local hits
  hits="$(grep -nE 'yoke-(ledger|workbench|docs|site)([^.a-z-]|$)|prj_structure|(^|[^a-z_])(specs|arch|testing)/[0-9]{2}|DECISIONS' "$root/README.md")"
  [[ -z "$hits" ]] || { echo "names a private repository or a design corpus: $hits"; return 1; }
}
