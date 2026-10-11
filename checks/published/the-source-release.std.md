# The family, as a source release

| | |
| --- | --- |
| **Feature** | what a consumer of the C family gets from its source release: `cmake --install` puts the library, its public headers, a CMake package config and a `pkg-config` file under a prefix; a program resolves the library by `find_package` or by `pkg-config` and links; the family states one version; and its release tag hands the source over |
| **Planning item** | yoke-project/yoke-sdk-c#26 |

## yoke-sdk-c:the-source-release.01 — installing puts the library, its headers, a package config and a pkg-config file under the prefix

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/97 §What C and C++ are consumed as |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout, built |
| **Action** | install it under an empty prefix |
| **Expected** | the prefix holds `include/yoke/base.h` and `include/yoke/plugin.h`, the library `libyoke-c.a`, `yoke-sdk-c-config.cmake` with its version file, and `yoke-sdk-c.pc`; no generated definition header is installed |

## yoke-sdk-c:the-source-release.02 — a CMake build resolves it with `find_package` and links

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/97 §What C and C++ are consumed as |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the family installed under a prefix, and a program calling `yoke_plugin_contract` |
| **Action** | configure the program with `find_package(yoke-sdk-c)` against that prefix, link it to `yoke::yoke-c`, and run it |
| **Expected** | it builds and prints the contract version the library speaks |

## yoke-sdk-c:the-source-release.03 — any other build resolves it with `pkg-config` and links

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/97 §What C and C++ are consumed as |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the family installed under a prefix, and the same program |
| **Action** | compile it with the flags `pkg-config --cflags --libs yoke-sdk-c` gives, and run it |
| **Expected** | it builds and prints the same contract version |

## yoke-sdk-c:the-source-release.04 — the family states one version

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/85 §The series · prj_structure/97 §What C and C++ are consumed as |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout |
| **Action** | read the version `CMakeLists.txt` states, the SDK line `base.h` defines, and the version the installed package config and pkg-config file carry |
| **Expected** | all four are the one version |

## yoke-sdk-c:the-source-release.05 — a release tag hands the source over

| Field | Value |
| --- | --- |
| **Cites** | prj_structure/85 §What a release is · prj_structure/97 §What C and C++ are consumed as |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a clean checkout |
| **Action** | read the `release` verb and the release workflow |
| **Expected** | the verb runs the published release verb with `-source yoke-sdk-c`, and the workflow runs it on a `v` tag and keeps the lines it prints |
