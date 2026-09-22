# The planning workflow

| | |
| --- | --- |
| **Feature** | a second workflow moves forward the planning item of everything a change names, and it verifies nothing, blocks nothing and is never depended on |
| **Planning item** | yoke-project/yoke-sdk-c#3 |

## yoke-sdk-c:planning-status.01 — the workflow runs on the work, and runs nothing of its own

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §Continuous integration |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout of this repository |
| **Action** | read the planning workflow: what starts it, and every command it runs |
| **Expected** | it starts on a proposed change and on a push to a branch that is not the default, and every command it runs is a script under `ci/`, given the event it was started by — a workflow that grew a step of its own would be a second place work happens |

## yoke-sdk-c:planning-status.02 — it is granted nothing, and with no credential it does nothing

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §Continuous integration |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout, and no credential in the environment |
| **Action** | read what the workflow grants its job, then run the script with no credential |
| **Expected** | the job is granted nothing beyond reading its own tree, and the script says no credential is configured and exits zero — an unconfigured repository is visibly unconfigured, and never a change that failed |

## yoke-sdk-c:planning-status.03 — an item moves forward, and never back

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §Continuous integration |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the script's own ordering of the states an item passes through |
| **Action** | ask it, for each state an item may be in — including none, which an item just added to the plan is in — what a push and a proposed change would move it to |
| **Expected** | an item with no state moves to either, and so does Todo; In Progress moves only to In Review; In Review and Done move to nothing, and no state moves to Done — the work's direction is the only thing this workflow knows, and a board it could move backwards would be worse than one nobody moves |

## yoke-sdk-c:planning-status.04 — the items are read from the change, and never invented

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §Continuous integration |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the event of a push whose commits name an item of this repository, an item of another, a number that is part of a word, and a commit naming none |
| **Action** | ask the script which items that push names |
| **Expected** | the two items, each qualified by its repository, and nothing else — a reference the forge would not resolve is not an item, and a workflow that guessed would move something nobody asked it to |
