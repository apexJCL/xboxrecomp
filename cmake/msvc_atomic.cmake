# MSVC proper (not clang-cl) has neither the __atomic/__sync builtins nor,
# without /experimental:c11atomics, C11 <stdatomic.h>. src/platform/
# msvc_atomic.h supplies the builtins on the Interlocked intrinsics; this
# force-includes it into every C file of the including project (and its
# subdirectories). Include it after project(). Under any other compiler it
# does nothing, and the header is empty there anyway.
if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C>:/FI${CMAKE_CURRENT_LIST_DIR}/../src/platform/msvc_atomic.h>"
        "$<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>")
endif()
