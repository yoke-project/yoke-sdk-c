# The interface library

| | |
| --- | --- |
| **Feature** | the C library an interface client is written with, over the same base and on the `local` projection: a channel's address computed from the instance's identity and the channel's name, or handed to a managed interface; attaching surfaces the opening picture, the standing subscription and the contract's version, and a version it does not speak is refused before any operation; one function per operation of the union, every request stating the contract's version, all on one connection and each answered on its own; a refusal as the base's error with its code and its detail, a suspension's grade and the channel it favours included; a subscription delivered in order until its caller ends it, and nothing reconnected; confirmation sent only when the author confirms; a stream's delivery read where its answer says it arrives — a socket or the connection — in order, until it ends; and closing an attachment ends it in order, so the Core reads a client that closed |
| **Planning item** | yoke-project/yoke-sdk-c#38 |

## yoke-sdk-c:the-interface-library.01 — a channel's address is computed from the instance's identity and the channel's name, or handed to a managed interface

| Field | Value |
| --- | --- |
| **Cites** | specs/25.24 · specs/70.19 · specs/70.21 · specs/90.6 · arch/70-interface-surface/01 §Where a channel is bound · arch/90-sdks/01 §One project, and what the base may hold |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the service form's runtime directory `/run/yoke`; an application instance `bench-a` under `XDG_RUNTIME_DIR=/run/user/1000`; a managed interface handed `YOKE_SOCKET`; a channel name holding a `/` |
| **Action** | compute the channel `panel`'s address in each, and the managed interface's |
| **Expected** | `/run/yoke/interfaces/panel.sock`; `/run/user/1000/yoke/bench-a/interfaces/panel.sock`; the socket `YOKE_SOCKET` names, and a managed interface handed none is refused naming it; the channel name holding a `/` is refused |

## yoke-sdk-c:the-interface-library.02 — attaching surfaces the opening, and a version it does not speak is refused before any operation

| Field | Value |
| --- | --- |
| **Cites** | specs/70.9 · specs/90.8 · arch/70-interface-surface/03 §The opening picture · arch/70-interface-surface/08 §How this contract states its version |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core whose opening carries one unit's record, the standing subscription `standing` and version 1; a second whose opening states version 2; a third that refuses the attachment `channel.in_use` |
| **Action** | attach to each |
| **Expected** | the first surfaces the unit's record, `standing` and version 1; the second is refused `compat.unsupported`, naming both versions, and the connection is let go; the third is the base's refusal carrying `channel.in_use` |

## yoke-sdk-c:the-interface-library.03 — one method per operation, each stating the contract's version, all on one connection and each answered on its own

| Field | Value |
| --- | --- |
| **Cites** | specs/70.12 · specs/90.8 · specs/90.10 · arch/70-interface-surface/04 §The eleven · arch/70-interface-surface/08 §`local` — the typed projection |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that records every request it is sent on one attachment, answers each, and answers a question about `slow` only after a command to `fast` |
| **Action** | call every operation the library offers; then ask `slow` and command `fast` at once |
| **Expected** | the eleven operations of the union were each sent once, every request stating the version 1, on the one attachment; the command and the question each return their own answer, the command's first |

## yoke-sdk-c:the-interface-library.04 — a refusal is the base's error with its code and its detail, a suspension's included

| Field | Value |
| --- | --- |
| **Cites** | specs/70.27 · specs/90.23 · arch/00-system/05 §How a refusal travels · arch/70-interface-surface/08 §This surface's error codes |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that refuses a read with `subject.unknown` naming the unit `nobody`, and a command with `channel.suspended` in grade `read-only` in favour of `service` |
| **Action** | read `nobody`; command a unit |
| **Expected** | the first is the base's refusal carrying `subject.unknown`, the kind `unit` and the identity `nobody`; the second is also the base's refusal, carrying `channel.suspended`, and surfaces the grade `read-only` and the channel `service` |

## yoke-sdk-c:the-interface-library.05 — a subscription is delivered in order until its caller ends it, and nothing reconnects

| Field | Value |
| --- | --- |
| **Cites** | specs/70.7 · specs/90.32 · arch/70-interface-surface/05 §What a subscription promises · arch/90-sdks/06 §It may not poll |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core whose standing subscription carries three events and then an overflow's fresh snapshot, and which answers a subscription with its snapshot and two events, then ends the attachment |
| **Action** | read the standing subscription; subscribe and read it; end the subscription; then call after the attachment has ended |
| **Expected** | the standing subscription delivers the three events in order and then the snapshot as an overflow; the subscription delivers its snapshot and its two events in order, and ending it cancels it on the wire; once the attachment has ended a call is answered that it ended, and the Core saw one attachment only |

## yoke-sdk-c:the-interface-library.06 — confirmation is sent only when the author confirms

| Field | Value |
| --- | --- |
| **Cites** | specs/70.32 · specs/70.33 · arch/70-interface-surface/03 §A connection that looks open proves nothing · arch/90-sdks/06 §It may not poll |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that records every request, whose standing subscription is `standing` |
| **Action** | attach and wait; then confirm the sequence 7 |
| **Expected** | nothing was sent while the author confirmed nothing; then one confirmation, of `standing` at 7 |

## yoke-sdk-c:the-interface-library.07 — a stream's delivery is read where its answer says it arrives, in order, until it ends

| Field | Value |
| --- | --- |
| **Cites** | specs/70.6 · arch/70-interface-surface/07 §The three delivery paths · arch/70-interface-surface/07 §What every path preserves |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that answers one subscription to a stream with a per-subscriber socket it listens on, and another with a delivery on the connection; and sends two frames on each before ending each |
| **Action** | subscribe to the stream twice and read each delivery; release a third |
| **Expected** | each delivery surfaces whether the stream is flowing, then the two frames in order, each with its sequence, its clock and its payload, then its end; releasing sends `stream.unsubscribe` naming the delivery |

## yoke-sdk-c:the-interface-library.08 — closing an attachment ends it in order, so the Core reads a client that closed

| Field | Value |
| --- | --- |
| **Cites** | specs/70.31 · arch/70-interface-surface/03 §What ends an attachment |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that records how each attachment's stream of requests ended |
| **Action** | attach, make one call, and close the attachment |
| **Expected** | the Core reads the end of the client's requests — the client closed — and not a connection that went away; `Close` returns once the attachment has ended |

## yoke-sdk-c:the-interface-library.09 — a panel built on the interface library names nothing of the administrative contract

| Field | Value |
| --- | --- |
| **Cites** | specs/90.5 · arch/90-sdks/01 §Three contracts, three libraries |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | this project |
| **Action** | read, comments left out, the header the interface library is written against and the sources it is built from |
| **Expected** | none names the administrative contract's definitions or the administrative library — a program written with the interface library cannot reach the operations an interface must never issue, because nothing in it names them |
