cmake_minimum_required(VERSION 3.25)
project(@YOKE_PROGRAM@ LANGUAGES C)

# This family's library, at the release this template came with: installed, or fetched and built beside
# the Plugin.
include(FetchContent)
# Fetched, the library is built alone: its harnesses and cases are its own repository's.
set(YOKE_SDK_C_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yoke-sdk-c
    GIT_REPOSITORY https://github.com/yoke-project/yoke-sdk-c.git
    GIT_TAG v0.7.1
    FIND_PACKAGE_ARGS 0.7.1)
FetchContent_MakeAvailable(yoke-sdk-c)

add_executable(@YOKE_PROGRAM@ main.c)
if(TARGET yoke::yoke-c)
    target_link_libraries(@YOKE_PROGRAM@ PRIVATE yoke::yoke-c)
else()
    target_link_libraries(@YOKE_PROGRAM@ PRIVATE yoke-c)
endif()
