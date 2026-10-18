# The interface harness

| | |
| --- | --- |
| **Feature** | the C interface library's harness: launched by the suite with the instance's root in `CONFORMANCE_INSTANCE`, it says hello for the interface contract, attaches to the channel a directive names at the address the library computes, turns each directive into one call of the library, reports what the library answered in the suite's vocabulary — an opening as its version, its standing subscription and its picture, a refusal as its code and what it names, a suspension's grade and channel included — and what the standing subscription or a subscription delivers as observations, in order; it judges nothing, confirms nothing it was not told to, and speaks no wire of its own |
| **Planning item** | yoke-project/yoke-sdk-c#38 |

## yoke-sdk-c:the-interface-harness.01 — hello first, for the interface contract

| Field | Value |
| --- | --- |
| **Cites** | specs/90.17 · arch/90-sdks/04 §The control protocol · arch/90-sdks/04 §How it is reached |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a control socket, and an instance binding the channel `panel` |
| **Action** | launch the harness with both in its environment |
| **Expected** | its first line is a hello naming the interface contract, the language `c`, the project's SDK line, the version 1, and no unit |

## yoke-sdk-c:the-interface-harness.02 — attaching reports the opening, and the standing subscription is observed in order

| Field | Value |
| --- | --- |
| **Cites** | specs/70.9 · arch/90-sdks/04 §The control protocol · arch/70-interface-surface/03 §The opening picture |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core whose channel `panel` opens at version 1 with the standing subscription `standing`, the sequence 4 and one channel's record, and carries two events on the standing subscription |
| **Action** | direct `attach` to `panel` |
| **Expected** | the result carries the version 1, `standing`, the sequence 4 and the record; then two observations `event`, in order, each carrying its type and its subject |

## yoke-sdk-c:the-interface-harness.03 — a refusal is reported as its code and what it names, a suspension's grade and channel included

| Field | Value |
| --- | --- |
| **Cites** | specs/90.23 · specs/70.27 · arch/90-sdks/04 §The control protocol · arch/00-system/05 §How a refusal travels |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the harness attached to a Core that refuses a read with `subject.unknown` naming the unit `nobody`, and a command with `channel.suspended` in grade `read-only` in favour of `bench` |
| **Action** | direct `read` of `nobody`, and `command` |
| **Expected** | the first result carries `subject.unknown` and the subject; the second `channel.suspended`, the grade `read-only` and the channel `bench`; neither carries the library's words |

## yoke-sdk-c:the-interface-harness.04 — a verb it does not know is unrecognised, finish ends it, and it holds no wire

| Field | Value |
| --- | --- |
| **Cites** | arch/90-sdks/04 §What a harness is · arch/90-sdks/04 §The vocabulary |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the harness, connected |
| **Action** | direct `emit`; then send finish; then read the harness's sources |
| **Expected** | `emit` is reported unrecognised; the harness exits zero; it speaks no wire: it includes no gRPC header and packs no message |

## yoke-sdk-c:the-interface-harness.05 — the suite is run against it, as it is against the other two

| Field | Value |
| --- | --- |
| **Cites** | specs/90.17 · specs/90.19 · arch/90-sdks/03 §The shape of a run |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | `ci/conformance.sh` |
| **Action** | read it |
| **Expected** | it builds the interface harness and runs the suite against it, beside the plugin and the administrative harnesses |
