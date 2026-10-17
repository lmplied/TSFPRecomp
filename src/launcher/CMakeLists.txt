# The launcher a recompiled title ships beside its game.
#
# Not a library and not built here: each title makes its own, because the
# executable it starts is the title's. A title's CMakeLists calls
#
#     recomp_add_launcher(timesplitters2_launcher timesplitters2_recomp.exe)
#
# (add NO_WIDESCREEN for a title where the widescreen options must not be
# offered; see below)
#
# and gets <name>.exe beside the game, reading the game's own XBE for the
# name it shows and the settings file it writes.
#
# Windows only for now. The drawing is behind launcher_theme.h so the day
# this needs to run elsewhere it is that file that is replaced, not the
# launcher.

if(WIN32)
    function(recomp_add_launcher TARGET GAME_EXE)
        add_executable(${TARGET} WIN32
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/launcher.c
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/launcher_theme.c
            # launcher_bindings.c is built into xbox_input, linked below,
            # which the in-game menu shares.
        )
        target_include_directories(${TARGET} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
        )
        target_compile_definitions(${TARGET} PRIVATE
            LAUNCHER_GAME_EXE="${GAME_EXE}"
        )
        # The launcher has to find the game's XBE where the game does, or it
        # names the wrong settings file and every choice made in it is lost.
        # The game looks beside itself in game/, then in YOUR_GAME_DIR from
        # its main.c; read that define rather than repeat it here, so the
        # two cannot drift. It is a C string literal, so its separators are
        # doubled backslashes: made forward slashes, which Windows accepts.
        set(_main_c "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
        if(EXISTS "${_main_c}")
            file(STRINGS "${_main_c}" _dir_line
                 REGEX "^#define[ \t]+YOUR_GAME_DIR[ \t]+\"")
            if(_dir_line MATCHES "\"([^\"]*)\"")
                string(REPLACE "\\\\" "/" _game_dir "${CMAKE_MATCH_1}")
                target_compile_definitions(${TARGET} PRIVATE
                    LAUNCHER_GAME_DIR="${_game_dir}")
            endif()
            # Re-configure when main.c changes, e.g. after regen_title_main.py.
            set_property(DIRECTORY APPEND PROPERTY
                CMAKE_CONFIGURE_DEPENDS "${_main_c}")
        endif()
        # xbox_config carries the settings file's format, so the launcher
        # and the runtime cannot disagree about what they are writing and
        # reading. xbox_input is the pads (recomp_pad.h: SDL3, or XInput),
        # the same code the game reads them with; the rest is in every Windows.
        target_link_libraries(${TARGET} PRIVATE
            xbox_config xbox_input user32 gdi32
        )
        # recomp_add_launcher(<target> <exe> NO_WIDESCREEN): a title where
        # widescreen cannot be right hides Widescreen and Wide camera, and
        # the launcher writes both off. A 2D fighter is the case: its stage
        # bounds and walls are built for 4:3, so a wider view shows space the
        # fighters cannot reach.
        # recomp_add_launcher(<target> <exe> NATIVE_WIDESCREEN): a title with a
        # 16:9 mode of its own, which it uses when the console says it is
        # widescreen. Widescreen is then all it takes, so Wide camera -- the
        # widening for a title without one -- is hidden and written off.
        # What else the title project supports, so the launcher offers it --
        # each matches something the project registers at start-up:
        #   ANY_ASPECT       xbox_D3D8SetTitleAnyAspect: Widescreen becomes
        #                    Off / 16:9 / Fill the window (with NATIVE_WIDESCREEN)
        #   FOV <degrees>    recomp_fov_support, with the game's own angle
        #   WEAPON_FOV <deg> recomp_weapon_fov_support, the same for the weapon
        # and how it looks:
        #   PALETTE <dark> <mid> <ink>   the title's colours, as RRGGBB hex,
        #                    in place of the Xbox green: the darks, a selected
        #                    panel, lines and text (xbox_D3D8MenuSetPalette
        #                    gives the in-game panel the same)
        #   PROMPTS          recomp_prompts_icon: the Button prompts row
        #   PAUSE_UNFOCUSED  recomp_bindings_pause_on_focus_loss
        #   SKIP_INTRO       xmv_set_intro_videos
        #   NETPLAY          hle_net_session_begin from its main: an Online tab
        #                    that hosts or joins (RECOMP_NET, hle_netplay.h)
        cmake_parse_arguments(_launcher
            "NO_WIDESCREEN;NATIVE_WIDESCREEN;ANY_ASPECT;PROMPTS;PAUSE_UNFOCUSED;SKIP_INTRO;NETPLAY"
            "FOV;WEAPON_FOV" "PALETTE" ${ARGN})
        if(_launcher_NO_WIDESCREEN)
            target_compile_definitions(${TARGET} PRIVATE LAUNCHER_NO_WIDESCREEN=1)
        endif()
        if(_launcher_NATIVE_WIDESCREEN)
            target_compile_definitions(${TARGET} PRIVATE LAUNCHER_NATIVE_WIDESCREEN=1)
        endif()
        foreach(_flag ANY_ASPECT PROMPTS PAUSE_UNFOCUSED SKIP_INTRO NETPLAY)
            if(_launcher_${_flag})
                target_compile_definitions(${TARGET} PRIVATE LAUNCHER_${_flag}=1)
            endif()
        endforeach()
        if(_launcher_NETPLAY)
            target_link_libraries(${TARGET} PRIVATE xbox_net)  # this PC's addresses
        endif()
        if(_launcher_FOV)
            target_compile_definitions(${TARGET} PRIVATE LAUNCHER_FOV=${_launcher_FOV})
        endif()
        if(_launcher_WEAPON_FOV)
            target_compile_definitions(${TARGET} PRIVATE
                LAUNCHER_WEAPON_FOV=${_launcher_WEAPON_FOV})
        endif()
        list(LENGTH _launcher_PALETTE _palette_count)
        if(_palette_count EQUAL 3)
            list(GET _launcher_PALETTE 0 _dark)
            list(GET _launcher_PALETTE 1 _mid)
            list(GET _launcher_PALETTE 2 _ink)
            target_compile_definitions(${TARGET} PRIVATE
                LAUNCHER_PALETTE_DARK=0x${_dark} LAUNCHER_PALETTE_MID=0x${_mid}
                LAUNCHER_PALETTE_INK=0x${_ink})
        elseif(_palette_count GREATER 0)
            message(FATAL_ERROR "recomp_add_launcher: PALETTE takes three RRGGBB colours")
        endif()
        if(MSVC)
            target_compile_options(${TARGET} PRIVATE
                /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
        endif()
    endfunction()
endif()
