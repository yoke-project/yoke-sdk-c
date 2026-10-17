# The conformance script, with a suite handed over

| | |
| --- | --- |
| **Feature** | `ci/conformance.sh` run by a caller that brings its own suite: the release of `yoke` runs this script at the commit this family's newest publication names, with the suite and the Core of its own commit, and joins the reports into the one its release carries. `CONFORMANCE_SUITE` names a directory holding `yoke-conformance` and `yoke-core`, which are then run instead of the published pair; `CONFORMANCE_REPORTS` names a directory where each harness's run writes its report |
| **Planning item** | yoke-project/yoke-sdk-c#32 |

## yoke-sdk-c:the-conformance-script.01 — a suite handed over is the one run, and nothing is downloaded

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/85 §The report a release carries · arch/90-sdks/03 §What a run produces |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a directory holding a stand-in `yoke-conformance` that writes down how it was called and a `yoke-core`; build tools that build nothing; the manifest and the downloads at an address nothing answers |
| **Action** | run `ci/conformance.sh` with `CONFORMANCE_SUITE` naming the directory |
| **Expected** | the script succeeds without reaching the manifest; the suite in the directory was run against its harness, given the directory's Core |

## yoke-sdk-c:the-conformance-script.02 — asked for reports, each harness's run writes one where asked

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/85 §The report a release carries · arch/90-sdks/03 §What a run produces |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | as `.01`, and an empty directory for the reports |
| **Action** | run `ci/conformance.sh` with `CONFORMANCE_SUITE` and `CONFORMANCE_REPORTS` |
| **Expected** | the directory holds one report per harness, named after it: `yoke-c-plugin-harness.json` |
