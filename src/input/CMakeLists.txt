# Xbox Input: the host's pads and keyboard, as an Xbox controller

add_library(xbox_input STATIC
    xinput_device.c
    # Host device choice and per-control bindings, from the config file the
    # input UI writes (tools/input_ui). Used by both input paths: the XAPI
    # replacement in src/hle and the USB gamepad in src/usb.
    input_bindings.c
    # The mouse (raw input on the game window) and the window's focus, which
    # decides whether the keyboard and mouse are the game's to read.
    input_mouse.c
    # The binding file as text, to read, change and write back: the launcher
    # edits it with this, and so does the in-game menu, so the two cannot
    # write it differently.
    ${CMAKE_CURRENT_SOURCE_DIR}/../launcher/launcher_bindings.c
    # The pads themselves: SDL3 everywhere, XInput on Windows behind
    # RECOMP_PAD_API=xinput or the bindings file's "pad_api".
    recomp_pad.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                             ${CMAKE_CURRENT_SOURCE_DIR}/../launcher)

# xbox_config: where the bindings file is, which is per title.
target_link_libraries(xbox_input PUBLIC platform xbox_config SDL3::SDL3)
if(WIN32)
    target_link_libraries(xbox_input PUBLIC xinput)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()

# What the runtime sees of the pads, live: `recomp_pad_probe [seconds] [xinput]`.
# Not built by default; `cmake --build <dir> --target recomp_pad_probe`.
add_executable(recomp_pad_probe EXCLUDE_FROM_ALL recomp_pad_probe.c)
target_link_libraries(recomp_pad_probe PRIVATE xbox_input)
