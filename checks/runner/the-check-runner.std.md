# The check runner

| | |
| --- | --- |
| **Feature** | `checks/run.sh`, which finds every check a marker names and reports it by that case: a check is found whatever its name holds, and a marker whose check the runner cannot find is reported as failing rather than left absent |
| **Planning item** | yoke-project/yoke-sdk-c#11 |

## yoke-sdk-c:the-check-runner.01 — a check whose name holds a digit is run

| Field | Value |
| --- | --- |
| **Cites** | testing/40 §From a test's result to its case |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a copy of the runner beside one check script, whose marker sits on a passing check named `check_l3_holds` |
| **Action** | run the copy |
| **Expected** | it reports `pass` for the case the marker names |

## yoke-sdk-c:the-check-runner.02 — a marker whose check cannot be found is reported as failing

| Field | Value |
| --- | --- |
| **Cites** | testing/40 §From a test's result to its case |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a copy of the runner beside one check script with two markers: one on a function the runner does not take for a check, `check_Missing`, and one at the end of the file, on nothing |
| **Action** | run the copy |
| **Expected** | it reports `FAIL` for both cases, saying no check follows the marker, and exits non-zero |
