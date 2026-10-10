# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    window_title.c
)
# The framebuffer window: GDI on Windows; on POSIX an SDL2 window that owns
# the process main thread (fb_present_sdl.c, xbox_HostWindowMain).
if(WIN32)
    target_sources(xbox_video PRIVATE fb_present.c)
    # The GDI window records keys in src/input's key table.
    target_link_libraries(xbox_video PUBLIC xbox_input)
else()
    find_package(SDL2 REQUIRED)
    target_sources(xbox_video PRIVATE fb_present_sdl.c)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2 xbox_input)
    if(APPLE)
        # The window's CAMetalLayer present for the Metal backend (manual
        # retain/release, like nv2a_pb_metal.m).
        target_sources(xbox_video PRIVATE fb_present_metal.m)
        target_link_libraries(xbox_video PUBLIC
            "-framework Metal" "-framework QuartzCore" "-framework CoreGraphics"
            "-framework Foundation")
    endif()
endif()
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
endif()
