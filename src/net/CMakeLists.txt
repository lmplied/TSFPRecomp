# This PC's addresses, for a player hosting co-op to give out: those on its
# own networks, and the public one, from STUN (recomp_net_addr.h). Shared by
# the launcher and the runtime (src/hle/hle_netplay.c), so both say the same.

add_library(xbox_net STATIC
    recomp_net_addr.c
    recomp_invite.c         # invite codes: the relay, the sealing, STUN on a socket
)

target_include_directories(xbox_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_net PUBLIC ws2_32 bcrypt winhttp)
endif()

if(MSVC)
    target_compile_options(xbox_net PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(xbox_net PRIVATE -Wall -Wextra -O2)
endif()
