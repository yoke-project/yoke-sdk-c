# The C plugin harness

| | |
| --- | --- |
| **Feature** | the thinnest translation between the suite's directives and the C plugin library: it says hello, turns each directive into a library call and each thing the library surfaces into an observation, reports a refusal as its code and a verb it does not know as unrecognised, holds no assertion, speaks no wire of its own, and exits when told to or when its Session has ended |
| **Planning item** | yoke-project/yoke-sdk-c#18 |

## yoke-sdk-c:the-harness.01 — hello first, with what the library declares

| Field | Value |
| --- | --- |
| **Cites** | specs/90.17 · specs/90.9 · arch/90-sdks/04 §How it is reached · arch/90-sdks/04 §The control protocol |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a control socket named by `CONFORMANCE_SOCKET`, and `YOKE_UNIT` set to `harness` |
| **Action** | start the harness |
| **Expected** | its first line is a hello carrying the contract `plugin`, the language `c`, the library's SDK line, the contract version the library declares, and the unit `harness` |

## yoke-sdk-c:the-harness.02 — describe answers with the Manifest the library generates

| Field | Value |
| --- | --- |
| **Cites** | specs/90.36 · arch/90-sdks/03 §The shape of a run · arch/90-sdks/06 §The declaration a plugin library produces |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a started harness |
| **Action** | issue `describe` |
| **Expected** | the result carries the directive's identifier and, as `manifest`, exactly the Manifest the library generates from the harness's declaration |

## yoke-sdk-c:the-harness.03 — start reports what admission answered, and a refusal as its code

| Field | Value |
| --- | --- |
| **Cites** | specs/90.23 · arch/90-sdks/04 §The control protocol · arch/90-sdks/05 §The observable model may not differ |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a plugin channel that accepts with restrictions; then one that refuses at authentication with `admission.auth.consumed` |
| **Action** | issue `start` against each |
| **Expected** | the first result says `accepted with restrictions` and names the granted and the withheld items; the second is a refusal carrying `admission.auth.consumed` and the stage `authentication`, and no message of the library's |

## yoke-sdk-c:the-harness.04 — a verb it does not know is reported as unrecognised

| Field | Value |
| --- | --- |
| **Cites** | specs/90.25 · arch/90-sdks/04 §The vocabulary |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a started harness |
| **Action** | issue `subscribe` |
| **Expected** | the result carries the directive's identifier and says the verb is unrecognised |

## yoke-sdk-c:the-harness.05 — what the library surfaces is an observation, in order, and the end ends the harness

| Field | Value |
| --- | --- |
| **Cites** | specs/90.29 · specs/90.23 · arch/90-sdks/04 §The control protocol · arch/90-sdks/06 §It may not hide the end of a Session |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a harness whose unit was started against a plugin channel |
| **Action** | the channel sends a command, then revokes the Session |
| **Expected** | the harness reports an observation of the command and then one of the end, with its cause, in that order, and then exits |

## yoke-sdk-c:the-harness.06 — finish ends the harness, and it holds no assertion and no wire

| Field | Value |
| --- | --- |
| **Cites** | specs/90.18 · arch/90-sdks/04 §What a harness is |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a started harness; the harness's source |
| **Action** | send `finish`; list the headers the harness includes and the libraries it links |
| **Expected** | the harness exits; it depends on neither the definitions nor a transport — only on the library under test |

## yoke-sdk-c:the-harness.07 — asked to stop, the harness reports its Session's end first, then leaves

| Field | Value |
| --- | --- |
| **Cites** | specs/90.29 · arch/90-sdks/06 §It may not hide the end of a Session · arch/35-units/04 §Ending one |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a harness whose unit was started against a plugin channel |
| **Action** | the process receives a termination signal, and the channel then revokes the Session |
| **Expected** | the harness reports the end of the Session and then exits zero |


## yoke-sdk-c:the-harness.08 — the suite is the published pair of binaries, authenticated, and never built

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/40 D5 · arch/90-sdks/04 §How it is reached |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the repository's conformance script and its workflow |
| **Action** | read how the script obtains the suite and the Core, and where the workflow runs it |
| **Expected** | it downloads `yoke`'s published conformance archive, compares its digest with the release manifest's line before using it, and builds nothing of `yoke`; the workflow runs it after `just test` |

## yoke-sdk-c:the-harness.09 — a question is observed with the bytes it carries

| Field | Value |
| --- | --- |
| **Cites** | specs/90.29 · arch/90-sdks/04 §The control protocol · arch/50-plugin-surface/05 §The eight |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a harness whose unit was started against a plugin channel |
| **Action** | the channel asks the unit a question carrying `how are you` |
| **Expected** | the harness reports an observation `question` with the question's identity, its type and its payload as the text sent — the suite requires the bytes, not only that a question arrived |

## yoke-sdk-c:the-harness.10 — it declares a stream on each transport, each governed by a capability

| Field | Value |
| --- | --- |
| **Cites** | specs/50.86 · arch/50-plugin-surface/07 §What the two tolerances select · arch/90-sdks/03 §A real Core, and no fixture |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | none |
| **Action** | read the harness's declaration |
| **Expected** | one stream that tolerates neither loss nor reorder, and one that tolerates loss, each governed by a capability of its own |
