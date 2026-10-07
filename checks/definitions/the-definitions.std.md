# The C definitions

| | |
| --- | --- |
| **Feature** | the C definitions are generated in this repository from the published definitions module, fetched at the version the record pins and authenticated by the digest the release manifest publishes for it, by the generators the record names; the generated tree is committed, and regenerating it changes nothing |
| **Planning item** | yoke-project/yoke-sdk-c#19 |

## yoke-sdk-c:the-definitions.01 — the record names what the tree was generated from, as the manifest publishes it

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/97 §The definitions, and the four families that are not Go · prj_structure/85 §The release record |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the committed `definitions/record`, and the release manifest |
| **Action** | read the record, and the manifest's publication lines |
| **Expected** | the record names the module `github.com/yoke-project/yoke/proto`, a version, an `h1:` digest, the versions of `protoc` and `protoc-c`, and the contracts generated; the manifest has a publication line for that module at that version carrying that digest |

## yoke-sdk-c:the-definitions.02 — a module whose digest is not the one published is refused, and nothing is generated

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/97 §The definitions, and the four families that are not Go · arch/90-sdks/01 §The five languages, and the party that is not a client |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a proxy serving a module zip, and a manifest whose publication line for it carries another digest |
| **Action** | run `ci/definitions.sh generate` against them, into an empty directory |
| **Expected** | it exits non-zero, names the digest it computed and the one published, and the directory stays empty |

## yoke-sdk-c:the-definitions.03 — a generator other than the one recorded is refused, naming both

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/97 §The definitions, and the four families that are not Go |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a `protoc` at the head of `PATH` that says it is `libprotoc 9.9.9` |
| **Action** | run `ci/definitions.sh check` |
| **Expected** | it exits non-zero, naming `9.9.9` and the version the record names |

## yoke-sdk-c:the-definitions.04 — the committed tree is what the recorded generators make of the recorded definitions

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/97 §The definitions, and the four families that are not Go · arch/00-system/05 §The encoding, and the framing |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the generators the record names, on `PATH` |
| **Action** | run `ci/definitions.sh check` |
| **Expected** | it exits zero: the module fetched is the one published, and generating every contract the record names, with the well-known types they import, gives the committed tree file for file |

## yoke-sdk-c:the-definitions.05 — a contract the generator cannot express is refused by name

| Field | Value |
| --- | --- |
| **Cites** | arch/90-sdks/02 §There is no partial SDK |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a record naming the contract `administrative`, whose proto3 `optional` fields `protoc-c` cannot express, and the recorded generators on `PATH` |
| **Action** | run `ci/definitions.sh generate` with it, into an empty directory |
| **Expected** | it exits non-zero naming the contract `administrative`; nothing is generated for it |

## yoke-sdk-c:the-definitions.06 — the generated tree compiles

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/95 §The verbs |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the committed tree, a C compiler and `libprotobuf-c`'s headers |
| **Action** | run `just build` |
| **Expected** | it exits zero, having compiled every generated source |
