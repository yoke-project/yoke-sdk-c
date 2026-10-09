# The base

| | |
| --- | --- |
| **Feature** | what the libraries of this project share and nothing else: connecting to a socket, the envelope and its correlation, a refusal as a C error structure carrying its code, the addresses a party may compute from its environment, and the contract version each library states |
| **Planning item** | yoke-project/yoke-sdk-c#17 |

## yoke-sdk-c:the-base.01 — the base holds nothing that exists on one contract only

| Field | Value |
| --- | --- |
| **Cites** | specs/90.6 · specs/90.7 · arch/90-sdks/01 §One project, and what the base may hold |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the base's source |
| **Action** | list every identifier of the definitions it refers to, and every header it includes |
| **Expected** | it refers to the envelope, the error family and the codes alone — no registration, no Session message, no surface, no heartbeat terms, no subscription — and it includes no header of a library of this project |

## yoke-sdk-c:the-base.02 — a refusal is a C error that carries its code, and envelopes are numbered and correlated

| Field | Value |
| --- | --- |
| **Cites** | specs/90.6 · specs/90.22 · specs/50.56 · specs/50.71 · arch/90-sdks/01 §One project, and what the base may hold · arch/50-plugin-surface/04 §Correlation |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | an error envelope carrying `session.correlation.unknown`; a sequence of envelopes built for one Session, one of them answering another |
| **Action** | turn the first into an error; build the sequence |
| **Expected** | the error is a refusal whose code is `session.correlation.unknown` and whose message is the envelope's; every envelope carries the Session's identity and a message identity none of the others has, and the answer's correlation is the identity of what it answers and never its own |

## yoke-sdk-c:the-base.03 — the addresses come from the environment, and nothing else is assumed

| Field | Value |
| --- | --- |
| **Cites** | specs/50.3 · specs/50.5 · arch/50-plugin-surface/01 §What the launch supplies · arch/90-sdks/01 §One project, and what the base may hold |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | an environment carrying the five variables; one missing `YOKE_SOCKET` |
| **Action** | read each |
| **Expected** | the first gives the plugin, the unit, the plugin channel's path, the path to bind and the token exactly as given; the second is an error naming `YOKE_SOCKET`, and no default path is supplied |
