# The record of a run

| | |
| --- | --- |
| **Feature** | a run leaves its results where the writer reads them, the record is assembled from them, and the workflow hands it over even when the run failed |
| **Planning item** | yoke-project/yoke-sdk-c#7 |

## yoke-sdk-c:the-record-of-a-run.01 — the verb undertakes to leave its results, whatever it decided

| Field | Value |
| --- | --- |
| **Cites** | testing/40 §Where records are kept · testing/40 §From a test's result to its case |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout |
| **Action** | read the `test` verb: what it writes, and what it does when something it runs fails |
| **Expected** | it writes the lines its checks wrote and the instants it started and finished — the form 470 names, and the two things no runner records about itself — and it carries a failure to its own exit rather than abandoning the rest, so a failing run leaves as much as a passing one. **This case reads the verb and never the results**: a check runs inside the run it would be inspecting, and one that read the directory would be reading either a half-written run or the leavings of the last one. That the leaving happens is `.04`'s artifact, in the run itself |

## yoke-sdk-c:the-record-of-a-run.02 — the record is assembled from them, and names what no runner knows

| Field | Value |
| --- | --- |
| **Cites** | testing/40 §A record |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a directory of results from a run in which everything passed |
| **Action** | run the script that assembles the record over it |
| **Expected** | one JSON record naming this repository, the commit it was run at, the level, the tier, the environment the run fixed and the tool that wrote it, with an entry for every case declared at that level and `state` passed — assembled from what the run left, and never from a second run |

## yoke-sdk-c:the-record-of-a-run.03 — the record of a failing run says so, and blocks

| Field | Value |
| --- | --- |
| **Cites** | testing/40 §A record · testing/20 §Blocking and not blocking |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a directory of results in which one blocking case failed |
| **Action** | run the script that assembles the record over it |
| **Expected** | the record is written all the same, `state` is failed and `blocks` is true — a record written only for a run that passed would make every report agree with itself by construction |

## yoke-sdk-c:the-record-of-a-run.04 — the workflow hands the record over, and can fail nothing

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §Continuous integration · testing/40 §Where records are kept |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout |
| **Action** | read the verify workflow: what assembles the record, what hands it over, and under which condition each runs |
| **Expected** | both run whatever the verbs decided, the assembling is a script under `ci/`, and the record leaves the run as an artifact — the record is evidence of what a run observed, so a failed run is exactly the one whose record is worth keeping |

## yoke-sdk-c:the-record-of-a-run.05 — nothing a run writes enters the tree

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout |
| **Action** | ask git whether what a run writes is ignored |
| **Expected** | it is, and nothing under it is ever reported as a change — a contributor who runs the verbs has nothing to undo and nothing to commit by mistake |
