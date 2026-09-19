# Continuous integration

| | |
| --- | --- |
| **Feature** | every proposed change to this repository runs its own verbs from a clean checkout — path filtering is `yoke`'s alone, so every change here runs every verb |
| **Planning item** | yoke-project/yoke-sdk-c#2 |

## yoke-sdk-c:continuous-integration.01 — every proposed change runs the verbs, and nothing else does work

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §Continuous integration |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout, and the repository's workflow |
| **Action** | read the workflow's triggers and every command it runs |
| **Expected** | it runs on every proposed change; `build`, `test`, `lint` and `fmt` are each run as `just <verb>`; and every other command is one of the repository's own scripts under `ci/` — the obligation lives in the repository, and the platform only calls it |

## yoke-sdk-c:continuous-integration.02 — what a run needs is declared, not taken from the runner

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/40 E1 |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout, and the repository's workflow |
| **Action** | read how the workflow obtains its machine and its tools |
| **Expected** | every job names a pinned runner image and never a moving one, and the runner and the language toolchain are installed at a stated version by the workflow itself — nothing a check needs is inherited from whatever an image happened to carry |
