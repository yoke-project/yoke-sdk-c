# The plugin library

| | |
| --- | --- |
| **Feature** | the library a Plugin unit is written with: one declaration that is both the Manifest it generates and the surface its registration claims; the first acts in their order — bind, register, open the Session — with the heartbeat on the Core's terms; everything the Session brings surfaced to the author, its end included; and the six things a library may not do |
| **Planning item** | yoke-project/yoke-sdk-c#17 |

## yoke-sdk-c:the-plugin-library.01 — a declaration generates the Manifest

| Field | Value |
| --- | --- |
| **Cites** | specs/90.36 · specs/42.6 · arch/90-sdks/06 §The declaration a plugin library produces · arch/50-plugin-surface/02 §The document |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a declaration of `com.yoke.station.acquire` needing `device:instrument`, with two streams — one tolerating loss and reorder — a command, a query, an occurrence and a capability governing each |
| **Action** | generate its Manifest |
| **Expected** | a YAML document carrying `manifest: 1`, the identity, the protocol this library declares, the need, the four lists with the tolerances written only where true, and each capability with the one object it governs |

## yoke-sdk-c:the-plugin-library.02 — nothing the model does not have can be declared

| Field | Value |
| --- | --- |
| **Cites** | specs/90.35 · specs/42.23 · arch/90-sdks/06 §The declaration a plugin library produces |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the declaration's types |
| **Action** | list every field an author can set on a declaration, a stream and a capability |
| **Expected** | none is an endpoint, an autostart flag, a digest or a description of a capability |

## yoke-sdk-c:the-plugin-library.03 — the registration claims what the Manifest declares, from the same value

| Field | Value |
| --- | --- |
| **Cites** | specs/90.38 · specs/50.16 · specs/90.9 · arch/90-sdks/06 §The declaration a plugin library produces · arch/50-plugin-surface/03 §What the request claims |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a unit started with the declaration of case 1, against a plugin channel that records what arrives |
| **Action** | start it |
| **Expected** | the request carries the plugin, the unit and the token from the environment, the protocol, the language `c` and this library's SDK line, and declared lists equal to the Manifest's — capabilities, streams, commands, queries and occurrences, all five; it carries no incarnation |

## yoke-sdk-c:the-plugin-library.04 — the unit's own socket is bound before it registers

| Field | Value |
| --- | --- |
| **Cites** | specs/50.6 · specs/90.23 · arch/50-plugin-surface/01 §The order of the first four acts · arch/90-sdks/05 §The observable model may not differ |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a plugin channel that, when a registration arrives, looks for a socket at `YOKE_BIND` |
| **Action** | start a unit |
| **Expected** | the socket was there when the registration arrived |

## yoke-sdk-c:the-plugin-library.05 — a refusal is surfaced with its stage and code, and never retried

| Field | Value |
| --- | --- |
| **Cites** | specs/90.30 · specs/50.23 · arch/90-sdks/06 §It may not retry admission |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a plugin channel that refuses at authentication with `admission.auth.consumed` |
| **Action** | start a unit |
| **Expected** | starting fails with a refusal whose stage is authentication and whose code is `admission.auth.consumed`; the channel saw one registration and no Session |

## yoke-sdk-c:the-plugin-library.06 — an acceptance with restrictions names what was withheld

| Field | Value |
| --- | --- |
| **Cites** | specs/50.26 · specs/50.27 · arch/50-plugin-surface/03 §Three outcomes |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a plugin channel that accepts with restrictions, granting one stream and withholding the other, and an occurrence |
| **Action** | start a unit |
| **Expected** | the unit reports it was admitted with restrictions, the granted scope and the withheld items by name |

## yoke-sdk-c:the-plugin-library.07 — the Session opens with the given identity, and beats on the Core's terms

| Field | Value |
| --- | --- |
| **Cites** | specs/50.47 · specs/50.53 · arch/50-plugin-surface/04 §Opening and closing · arch/50-plugin-surface/04 §Revocation |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a plugin channel that accepts with a heartbeat interval of 100 ms |
| **Action** | start a unit whose author reports its health once, as soon as it starts, and wait 450 ms |
| **Expected** | the Session's first envelope is an `OPEN` carrying the identity admission issued; at least three heartbeats followed it, no two closer than 50 ms; the author was given no way to choose the interval |

## yoke-sdk-c:the-plugin-library.08 — the end of a Session is surfaced, and nothing reconnects

| Field | Value |
| --- | --- |
| **Cites** | specs/90.29 · specs/50.41 · specs/50.103 · arch/90-sdks/06 §It may not hide the end of a Session |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | an open Session |
| **Action** | the channel revokes it for a disabled plugin, with a line |
| **Expected** | the unit surfaces the end, with the cause and the line, and reports itself done; the channel sees no further registration and no further Session |

## yoke-sdk-c:the-plugin-library.09 — an orderly close is the unit's

| Field | Value |
| --- | --- |
| **Cites** | specs/50.49 · arch/50-plugin-surface/04 §Opening and closing |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | an open Session |
| **Action** | the author closes it |
| **Expected** | the channel receives a session `CLOSE`, and the unit surfaces its end as a close it made |

## yoke-sdk-c:the-plugin-library.10 — what the Core sends is surfaced, and an answer is correlated to it

| Field | Value |
| --- | --- |
| **Cites** | specs/50.69 · specs/50.72 · specs/90.23 · arch/50-plugin-surface/04 §Correlation |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | an open Session |
| **Action** | the channel sends a command `calibrate` and then a question `head-status`; the author acknowledges the one and answers the other |
| **Expected** | both are surfaced in the order they were sent; the acknowledgement and the answer arrive correlated to the command's and the question's message identities |

## yoke-sdk-c:the-plugin-library.11 — an occurrence carries the author's severity, or is refused

| Field | Value |
| --- | --- |
| **Cites** | specs/90.34 · arch/90-sdks/06 §It may not choose a severity on an author's behalf |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | an open Session |
| **Action** | report `calibration.drift` with no severity; then with severity 40 |
| **Expected** | the first is refused by the library and nothing is sent; the second arrives as an event with severity 40 |

## yoke-sdk-c:the-plugin-library.12 — nothing is emitted on a stream the Core has not activated

| Field | Value |
| --- | --- |
| **Cites** | specs/90.33 · specs/90.32 · arch/90-sdks/06 §It may not create a stream's transport |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | an open Session, and no stream activated |
| **Action** | emit on `station.spectra` |
| **Expected** | the library refuses it with `stream.inactive`; no socket was created for the stream and nothing reached the channel |

## yoke-sdk-c:the-plugin-library.13 — every family a unit originates has an act, the error family among them

| Field | Value |
| --- | --- |
| **Cites** | specs/50.61 · specs/50.68 · specs/50.102 · specs/90.10 · arch/50-plugin-surface/05 §The eight · arch/50-plugin-surface/05 §Direction is a rule of its own |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | an open Session, and a command and a question the channel sent |
| **Action** | the author accepts the command and then says it is done; says what went wrong with the question, with a code and a message |
| **Expected** | two acknowledgements correlated to the command, `accepted` then `done`; an error correlated to the question, carrying the code and the message |

## yoke-sdk-c:the-plugin-library.14 — a beat repeats the author's last report, and says nothing before the first

| Field | Value |
| --- | --- |
| **Cites** | specs/50.65 · specs/50.66 · specs/90.34 · arch/50-plugin-surface/05 §What a health report carries · arch/90-sdks/06 §It may not choose a severity on an author's behalf |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a plugin channel that accepts with a heartbeat interval of 100 ms |
| **Action** | start a unit and wait 350 ms; the author reports 40 with a line, and 350 ms pass; the author reports 10 with another line, and 350 ms pass |
| **Expected** | no health report reaches the channel before the author's first; after it, at least two beats repeat 40 and its line; from the second on, at least two beats repeat 10 and its line, and nothing older follows it; no report carries a grade or a line the author did not state |

## yoke-sdk-c:the-plugin-library.15 — an activation connects the library to the transport it names, and is acknowledged

| Field | Value |
| --- | --- |
| **Cites** | specs/50.86 · specs/90.33 · arch/50-plugin-surface/07 §A stream flows because it was told to · arch/50-plugin-surface/08 §The other transports |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | an open Session; a packet socket and a datagram socket the test listens on, and an address nothing listens on |
| **Action** | the channel activates one stream on the ordered transport, one on the framed transport, and one at the address nothing listens on |
| **Expected** | the library connects to each socket that listens and acknowledges each activation as done, and the author is handed each activation; the third is acknowledged as failed, with a line saying why, and that stream stays inactive |

## yoke-sdk-c:the-plugin-library.16 — Emit writes one data envelope per packet, or one frame per datagram, numbered from 1

| Field | Value |
| --- | --- |
| **Cites** | specs/50.90 · arch/50-plugin-surface/07 §The frame · arch/50-plugin-surface/08 §The other transports · arch/00-system/05 §The encoding, and the framing |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | one stream activated on each transport |
| **Action** | emit three payloads on each |
| **Expected** | the ordered socket reads three data envelopes, sequences 1 to 3, each with a message identity, the Session's identity, a send time and the payload unchanged; the datagram socket reads three frames, each a little-endian header of sequence 1 to 3 and a clock, then the payload unchanged; nothing reached the Session |

## yoke-sdk-c:the-plugin-library.17 — a stop closes the transport, is acknowledged, and Emit is refused after it

| Field | Value |
| --- | --- |
| **Cites** | specs/90.33 · arch/50-plugin-surface/07 §Three routes end a stream, and the transport goes in all three · arch/90-sdks/06 §It may not create a stream's transport |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | one stream activated on the ordered transport |
| **Action** | the channel stops it; the author emits on it |
| **Expected** | the library closes its connection, acknowledges the stop as done, and hands the author the stop; the emission is refused with `stream.inactive`, and nothing reaches the socket |
