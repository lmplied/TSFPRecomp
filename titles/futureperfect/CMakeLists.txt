cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Timesplitters - Future Perfect - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(futureperfect_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# ── Bring-up diagnostics ────────────────────────────────────────
#
# ebx/esi/edi are callee-saved on x86 and the recompiler keeps them in globals,
# so a function whose epilogue did not lift correctly corrupts its *caller*.
# There is no crash and no message where the failure happens: the caller
# carries on with a wrong loop cursor or a wrong `this` and faults somewhere
# unrelated, often several frames away.
#
# With this on, every call -- direct and indirect -- checks that ebx, esi and
# edi came back unchanged and that esp came back at least 4 higher, and names
# the offending function. Off by default because it costs a few compares per
# call; leave it on until a title boots.
#
# Directory scope, and set before add_subdirectory, deliberately: the check is
# emitted into generated game code but recomp_abi_violation_log() is compiled
# into xbox_kernel. Defining it on the game target alone builds the calls
# without the callee and fails to link.
option(RECOMP_ABI_CHECK "Verify callee-saved registers across every call" OFF)
if(RECOMP_ABI_CHECK)
    add_compile_definitions(RECOMP_ABI_CHECK)
    message(STATUS "ABI check enabled: callee-saved registers verified per call")
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # A PDB in every configuration, so the crash handler's SymFromAddr can
        # name the generated function a host RIP is in. Linker-only: without
        # /Zi it holds public symbols -- every sub_XXXXXXXX -- and costs no
        # recompile of the large generated files.
        /DEBUG
    )
    # The generated code files can be very large. Increase object file section
    # limit, and compile them in parallel: MSBuild only parallelises across
    # projects, so without /MP the 36 generated files of a 36,000-function
    # title compile one after another on a single core.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /MP)

    # Drop MSVC's Debug runtime checks. /RTC1 traps a local read before its
    # first write, and lifted code does that deliberately: an fpo_leaf function
    # inherits its caller's frame, so it reads ebp before assigning it and
    # picks the value up from g_seh_ebp. That is correct by design here, but
    # /RTC1 cannot know it and aborts through a message box -- which never
    # reaches stderr, so the run looks like a silent hang at the first lifted
    # frame. Debug is the configuration bring-up uses, so this cannot stay on.
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

# ── Keyboard and mouse ──────────────────────────────────────────
# Future Perfect's own layout for a keyboard and mouse, beside the
# executable, where the runtime reads it when the player has no bindings
# of their own for this title (recomp_bindings_read_path) and where the
# launcher's "Reset this controller" goes back to. WASD to move, the mouse
# to look, left click fires, right click aims; the full table is in
# docs/technical/input-binding.md.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/input_bindings.json
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/input_bindings.json)

# ── Launcher ────────────────────────────────────────────────────
# A settings window that writes the per-title settings and controls and
# starts the game. Windows only; see "The launcher" in the top-level README.
if(COMMAND recomp_add_launcher)
    # NATIVE_WIDESCREEN: Future Perfect has a 16:9 mode of its own, chosen
    # from the console's video flag (XGetVideoFlags, read in its device
    # set-up at 0x00023A91), so the launcher offers Widescreen alone.
    recomp_add_launcher(${PROJECT_NAME}_launcher ${PROJECT_NAME}.exe NATIVE_WIDESCREEN)
    # The same preset beside the launcher, for its reset.
    add_custom_command(TARGET ${PROJECT_NAME}_launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/input_bindings.json
                $<TARGET_FILE_DIR:${PROJECT_NAME}_launcher>/input_bindings.json)
endif()
