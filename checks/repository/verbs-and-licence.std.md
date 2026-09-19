# The six verbs, the container and the licence
| | |
| --- | --- |
| **Feature** | this repository defines the six verbs, builds itself in a container it defines, and carries the licence its role assigns |
| **Planning item** | yoke-project/yoke-sdk-c#1 |

## yoke-sdk-c:verbs-and-licence.01 — every verb is defined, and none is silent

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout of this repository, with `just` on the path |
| **Action** | ask `just` for the recipes the repository defines, and read the body of each of `build`, `test`, `lint`, `fmt`, `develop` and `release` without running it |
| **Expected** | all six are defined and no body is empty — a verb with nothing to do has a body that says so, because a missing verb and a silent one are indistinguishable to a fan-out that skips |

## yoke-sdk-c:verbs-and-licence.02 — nothing resolves a path outside the repository

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/40 E1 · prj_structure/40 E2 |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout |
| **Action** | read the `justfile` and the module files for a path that leaves the repository's own tree |
| **Expected** | none is found — what a check needs is stated in the repository, and a sibling reached by a relative path is how a workspace becomes a build input |

## yoke-sdk-c:verbs-and-licence.03 — the repository builds in the container it defines itself

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs · prj_structure/40 E4 |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout and a container engine; no language toolchain taken from the host |
| **Action** | build the image this repository's `Containerfile` defines, and run `build` inside it |
| **Expected** | the image builds and `build` exits zero inside it — the container is this repository's, so a contributor without the toolchain can still build |

## yoke-sdk-c:verbs-and-licence.04 — the licence is the one this repository's role assigns

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/40 E3 · prj_structure/95 §The licences |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout |
| **Action** | read the licence and notice files at the repository's root |
| **Expected** | the licence is **Apache 2.0**, the one rule 1 assigns to software; a `NOTICE` names the copyright holder; and no second licence file stands beside them — the check tests the assignment and not a file's existence |

## yoke-sdk-c:verbs-and-licence.05 — `develop` fails on a runner older than the floor

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout, and a floor above the `just` that is installed |
| **Action** | run `develop`, given that floor |
| **Expected** | it exits non-zero, naming the floor it was given and the version it found |

## yoke-sdk-c:verbs-and-licence.06 — `fmt` fails on an unformatted tree

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a copy of the checkout with one C file left unformatted |
| **Action** | run `fmt` on the copy |
| **Expected** | it exits non-zero and names the file, so that formatting is a check and not a habit |
