# The six verbs every repository defines.
# A verb with nothing to do says so in one line, so a fan-out can tell a gap from a statement.

# Build this repository's codebase.
build:
    @echo "build: nothing to build in yoke-sdk-c yet"

# Run this repository's own checks, with no sibling present.
test:
    #!/usr/bin/env bash
    # A run leaves its results where the record writer reads them, whatever it decided.
    set -uo pipefail
    mkdir -p .results
    date -u +%Y-%m-%dT%H:%M:%SZ > .results/started
    status=0
    if command -v yoke-verify > /dev/null; then
        yoke-verify descriptions --repository yoke-sdk-c . > /dev/null || status=1
        yoke-verify markers --repository yoke-sdk-c . > /dev/null || status=1
    else
        echo "test: yoke-verify is not on PATH; \`just develop\` puts it there"
        status=1
    fi
    bash checks/run.sh | tee .results/checks.txt || status=1
    date -u +%Y-%m-%dT%H:%M:%SZ > .results/finished
    exit "$status"

# This repository's static checks.
lint:
    #!/usr/bin/env bash
    set -euo pipefail
    shopt -s nullglob
    bash -n checks/run.sh checks/*/*.sh ci/*.sh
    echo "lint: every shell script parses"

# Fail, naming each file, when the tree is not formatted.
fmt:
    #!/usr/bin/env bash
    set -euo pipefail
    files="$(find . \( -name '*.c' -o -name '*.h' \) -not -path './.git/*' | sort)"
    if [[ -z "$files" ]]; then echo "fmt: nothing to format yet"; exit 0; fi
    clang-format --dry-run -Werror $files
    echo "fmt: every file is formatted"

# Verify the toolchain against the floor the workspace's fan-out passes, and put the verification
# tool on PATH at the version the workspace names — run alone, the newest published.
develop floor="" verify="":
    #!/usr/bin/env bash
    set -euo pipefail
    found="$(just --version | awk '{print $2}')"
    if [[ -z "{{floor}}" ]]; then
        echo "develop: no floor given, so none verified — the workspace passes it; found just $found"
    elif ! [[ "{{floor}}" =~ ^[0-9]+(\.[0-9]+)*$ ]]; then
        echo "develop: '{{floor}}' is not a version; pass it as \`just develop 1.58.0\`"
        exit 1
    else
        lowest="$(printf '%s\n%s\n' "{{floor}}" "$found" | sort -V | head -n 1)"
        if [[ "$lowest" != "{{floor}}" ]]; then
            echo "develop: just {{floor}} or newer is needed; found just $found"
            exit 1
        fi
        echo "develop: just $found meets the floor {{floor}}"
    fi
    bash ci/yoke-verify.sh "{{verify}}"

# Publish into this repository's ecosystem, one manifest line per publication.
release:
    @echo "release: nothing to publish from yoke-sdk-c yet"
