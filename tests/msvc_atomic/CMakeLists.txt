cmake_minimum_required(VERSION 3.20)
project(msvc_atomic_test C)
# src/platform/msvc_atomic.h: every __atomic/__sync builtin the runtime uses,
# at each object width, on integers, pointers and floating point. Under GCC
# and Clang the same test runs against the real builtins, so both sides are
# held to one expectation.
set(CMAKE_C_STANDARD 11)
include(../../cmake/msvc_atomic.cmake)
add_executable(msvc_atomic_test test_main.c)
enable_testing()
add_test(NAME msvc_atomic_test COMMAND msvc_atomic_test)
