cmake_minimum_required(VERSION 3.20)
project(apu_monitor_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_monitor_test test_main.c)
target_include_directories(apu_monitor_test PRIVATE ../../src ../../src/apu)
# The whole runtime, as apu_mixdown: the APU delivers its interrupt and
# resolves physical addresses through the kernel. test_main.c defines every
# xa2_* function, so the archive's apu_xaudio2.o is never pulled in and the
# monitor submits to the fake device below.
target_link_libraries(apu_monitor_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_monitor COMMAND apu_monitor_test)
set_tests_properties(apu_monitor PROPERTIES ENVIRONMENT "RECOMP_APU_TRACE=1")
