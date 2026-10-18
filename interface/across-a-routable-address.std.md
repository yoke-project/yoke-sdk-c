# The interface library across a routable address

| | |
| --- | --- |
| **Feature** | the C interface library reaching a channel bound at a routable address: TLS 1.3 and nothing below it, the Core's chain verified against the roots its caller supplies and no others, ALPN `h2`, no client certificate; then the credential, as the attachment's first frame and before anything else — an account and its secret, or a session token an earlier authentication issued — its answer surfaced as the account established, the token and its expiry, and a refusal as the base's error; the token the caller's to carry into a later attachment, the library reconnecting nothing |
| **Planning item** | yoke-project/yoke-sdk-c#38 |

## yoke-sdk-c:across-a-routable-address.01 — the library dials TLS 1.3 alone, against the roots its caller supplies

| Field | Value |
| --- | --- |
| **Cites** | arch/80-security/03 §TLS 1.3 and nothing below it · arch/80-security/03 §What the two projections look like on a routable address · arch/80-security/03 §The caller's credential sits above the transport |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core on loopback over TLS presenting a chain issued by a root `A`, which records the version and protocol each connection negotiated and whether a client certificate was offered; a second capped at TLS 1.2; a third presenting a chain issued by a root `B` |
| **Action** | attach to the first with `A` as the roots; to the second with `A`; to the third with `A`; with no roots; and to a local socket asking for TLS |
| **Expected** | the first attaches, the Core reading TLS 1.3, ALPN `h2` and no client certificate; the second and third are refused at the handshake and nothing is attached; no roots, and TLS on a local socket, are refused before anything is dialled |

## yoke-sdk-c:across-a-routable-address.02 — the credential is the attachment's first frame, and its answer is surfaced

| Field | Value |
| --- | --- |
| **Cites** | arch/70-interface-surface/03 §Who the client is · arch/80-security/04 §The session token · arch/70-interface-surface/08 §`local` — the typed projection |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a routable Core over TLS that reads the first frame before it opens: it accepts `alice` with `hunter2x` and answers a token and an expiry, accepts that token again, and refuses anything else `auth.invalid` |
| **Action** | attach presenting `alice` and `hunter2x`; attach presenting the token the first answer carried; attach presenting `alice` and another secret |
| **Expected** | in the first two the Core's first frame received is `authenticate` stating the contract's version, the first carrying the password and the second the token; each surfaces the opening and the account `alice` with the token and expiry the Core answered; the third is the base's refusal carrying `auth.invalid`, and nothing is attached |
