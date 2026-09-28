# The verification tool, published

| | |
| --- | --- |
| **Feature** | `develop` puts the published `yoke-verify` on `PATH` — at the version the workspace names, or the newest the release manifest names when run alone — authenticated by its manifest line; `test` checks the descriptions and the markers with it; continuous integration reaches it through `develop`, and installs no Go; and the record names the tool by what the binary says it is |
| **Planning item** | yoke-project/yoke-sdk-c#14 |

## yoke-sdk-c:the-published-tool.01 — `develop` puts the tool on `PATH` at the version given, authenticated by its line

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs · prj_structure/85 §The release record |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a release manifest naming `yoke-verify` `9.9.9` for this machine's architecture with the digest of an archive served beside it, and an empty user `bin` directory at the head of `PATH` |
| **Action** | run `just develop "" 9.9.9` against them |
| **Expected** | it exits zero; the archive's `yoke-verify` is in the user's `bin` directory and is the one `PATH` finds; and `develop` names `9.9.9` as the version given |

## yoke-sdk-c:the-published-tool.02 — given no version, `develop` takes the newest the manifest names, and says so

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a release manifest naming `yoke-verify` `9.9.8` and `9.9.10`, each with its archive, and an empty user `bin` directory at the head of `PATH` |
| **Action** | run `just develop` with no parameter |
| **Expected** | it exits zero with `9.9.10` installed — the newest by version and not by text — and says that none was given |

## yoke-sdk-c:the-published-tool.03 — an archive that is not the one its line names is never installed

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs · prj_structure/85 §The release record |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a release manifest whose line for `yoke-verify` `9.9.9` names a digest that the archive served does not have |
| **Action** | run `just develop "" 9.9.9` against it |
| **Expected** | it exits non-zero, naming the difference, and the user `bin` directory holds no `yoke-verify` |

## yoke-sdk-c:the-published-tool.04 — `test` checks the descriptions and the markers with the tool on `PATH`

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs · testing/30 §What checks a description |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout |
| **Action** | read the body of `test` without running it |
| **Expected** | it runs `yoke-verify descriptions` and `yoke-verify markers` on this repository, names the tool by its name alone and never by a path, a module or a version, and fails saying so when `PATH` does not reach it |

## yoke-sdk-c:the-published-tool.05 — continuous integration reaches the tool through `develop`, and installs no Go

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §Continuous integration · prj_structure/40 E4 |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout, and the repository's workflow |
| **Action** | read the workflow's steps |
| **Expected** | `just develop` runs before `just test`; and no step installs Go, since this repository's language is not Go and a check that is not about a language needs no toolchain for it |

## yoke-sdk-c:the-published-tool.06 — the record names the tool by the build information the binary carries

| Field | Value |
| --- | --- |
| **Cites** | testing/40 §A record |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a `yoke-verify` on `PATH` whose build information names `github.com/yoke-project/yoke` at `v9.9.9` and a revision, and the results a run leaves |
| **Action** | run `ci/record.sh` |
| **Expected** | the tool is asked to write the record with `--ran yoke-verify=v9.9.9@` and the revision's first twelve characters — the version the binary states, and nothing learnt from where it was installed |
