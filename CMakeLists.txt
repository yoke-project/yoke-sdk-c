# The C family: the base and the plugin library, over the committed definitions, with gRPC through its
# core C API.
cmake_minimum_required(VERSION 3.25)
project(yoke-sdk-c VERSION 0.4.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc)
pkg_check_modules(PROTOBUF_C REQUIRED IMPORTED_TARGET libprotobuf-c)
find_package(Threads REQUIRED)

# The generated definitions, compiled as they were generated.
file(GLOB_RECURSE DEFINITIONS CONFIGURE_DEPENDS definitions/*.c)
set_source_files_properties(${DEFINITIONS} PROPERTIES COMPILE_OPTIONS "-Wall;-Werror")

add_library(yoke-c base/base.c plugin/plugin.c ${DEFINITIONS})
target_include_directories(yoke-c
    PUBLIC include definitions
    PRIVATE base)
target_link_libraries(yoke-c PUBLIC PkgConfig::GRPC PkgConfig::PROTOBUF_C Threads::Threads)
target_compile_definitions(yoke-c PRIVATE _GNU_SOURCE)
set_source_files_properties(base/base.c plugin/plugin.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

# The cases, each program reporting its own in the form the record writer reads.
add_library(yoke-c-testing STATIC testing/check.c testing/channel.c)
target_include_directories(yoke-c-testing PUBLIC testing)
target_link_libraries(yoke-c-testing PUBLIC yoke-c)
target_compile_options(yoke-c-testing PRIVATE -Wall -Wextra -Werror)

foreach(program base/base_test plugin/plugin_test)
    get_filename_component(name ${program} NAME)
    add_executable(${name} ${program}.c)
    target_include_directories(${name} PRIVATE base)
    target_compile_definitions(${name} PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(${name} PRIVATE yoke-c-testing)
endforeach()

# The harness the conformance suite drives, and its cases. It speaks to the suite in JSON, through
# json-c, and to the Core only through the library under test.
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
add_library(yoke-c-harness STATIC harness/harness.c)
target_include_directories(yoke-c-harness PUBLIC harness)
target_compile_options(yoke-c-harness PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(yoke-c-harness PRIVATE _GNU_SOURCE)
target_link_libraries(yoke-c-harness PUBLIC yoke-c PkgConfig::JSONC Threads::Threads)
add_executable(yoke-c-plugin-harness harness/main.c)
target_link_libraries(yoke-c-plugin-harness PRIVATE yoke-c-harness)
add_executable(harness_test harness/harness_test.c)
target_compile_definitions(harness_test PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}" _GNU_SOURCE)
target_compile_options(harness_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(harness_test PRIVATE yoke-c-harness yoke-c-testing)
