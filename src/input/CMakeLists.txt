# Xbox Input → XInput Compatibility Layer
#   input_map.c     the host -> Duke mapping table and pure helpers
#   input_core.c    public API, connection cache, probe backoff
#   xinput_device.c host backends (XInput on Windows, SDL2 elsewhere)
#   keyboard.c      the key table and the one key map (every host)
#   keyboard_sdl.c  the SDL window's key events into that table (POSIX)

add_library(xbox_input STATIC
    input_map.c
    input_core.c
    xinput_device.c
    keyboard.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput)
else()
    find_package(SDL2 REQUIRED)
    target_sources(xbox_input PRIVATE keyboard_sdl.c)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2 m)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()

# The mapping table, deadzone and connection cache, against a fake backend
# (every host, no device needed).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_map
                 ${CMAKE_CURRENT_BINARY_DIR}/input_map)
# The key map and its README table (every host, no SDL), and the SDL key
# and focus rules against synthetic events (POSIX).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_keyboard
                 ${CMAKE_CURRENT_BINARY_DIR}/input_keyboard)
if(NOT WIN32)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_keyboard_sdl
                     ${CMAKE_CURRENT_BINARY_DIR}/input_keyboard_sdl)
endif()
