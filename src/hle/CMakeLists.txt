# XDK functions replaced by name (see hle.h and tools/recomp/hle.py).
#
# Implementations are marked HLE_EXPORT(Name). tools.recomp scans this
# directory for the markers and generates the thunks that reach them, so a
# function added here is picked up at the next lift with no other edit.
#
# The DirectSound replacement (hle_dsound.c and the files it uses) is adapted
# from doaxbv-re (https://github.com/NoRain211/doaxbv-re), GPL-3.0.

if(MSVC)
    # /Zi so the PDB names this library's static functions in a sampled
    # profile (RECOMP_SAMPLE); see src/kernel/CMakeLists.txt.
    add_compile_options(/Zi)
endif()

add_library(xbox_hle STATIC
    hle_d3d8.c
    hle_d3d8_state.c
    hle_d3d8_texture.c
    hle_d3d8_vertex.c
    hle_d3d8_record.c
    hle_d3d8_interp.c
    d3d8_capture.c
    hle_dsound.c
    hle_dsound_stream.c
    dsound_buffer_model.c
    xbox_adpcm.c
    hle_input.c
    hle_xmv.c
    hle_xmv_play.c
    input_model.c
)

# Host backends. XAudio2 and XInput exist only on Windows. A POSIX host needs
# its own recomp_audio_output_* (audio_output.h) and recomp_input_host_sample
# (input_host.h) -- SDL2 is the obvious candidate, since xbox_apu and
# xbox_input already use it. Until those exist the library compiles on POSIX,
# but a title that links it there will be missing those symbols.
if(WIN32)
    target_sources(xbox_hle PRIVATE
        audio_output_xaudio2.cpp
        input_host.c
        # The in-game settings menu (F1): what it shows and does. The panel
        # itself is drawn by src/d3d/d3d8_menu.c.
        hle_menu.c
        # F12: the picture on screen, as a PNG in Pictures.
        hle_screenshot.c
        # Button prompts for the keyboard and mouse: the pad's icons drawn
        # as the keys bound to them (hle_texture_replace.h).
        hle_prompts.c
        hle_lockstep.c
        hle_netplay.c
    )
endif()
target_include_directories(xbox_hle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_hle PUBLIC xbox_config)
target_link_libraries(xbox_hle PUBLIC xbox_kernel)
# Host device choice and per-control bindings (src/input/input_bindings.c),
# shared with the USB gamepad so one config file covers both input paths.
target_link_libraries(xbox_hle PUBLIC xbox_input)
target_link_libraries(xbox_hle PUBLIC xbox_net)       # co-op: this PC's addresses
if(WIN32)
    target_link_libraries(xbox_hle PRIVATE xaudio2 ole32 oleaut32 xinput9_1_0 shell32 uuid ws2_32)
    # The D3D8 replacements drive the host renderer in src/d3d, and open a
    # window of their own for it.
    target_link_libraries(xbox_hle PRIVATE xbox_d3d8 xbox_video user32)
endif()
