/*
 * recomp_net_addr.h -- this PC's addresses, for a player hosting co-op to
 * give out: the ones on its own networks, and the one the internet sees.
 *
 * Shared by the launcher (its Co-op tab) and the runtime (the host's waiting
 * window, the in-game Co-op page), so the two always say the same.
 *
 * The public address is asked of a public STUN server -- the way video calls
 * find theirs: one small UDP packet out, one back, with nothing of this PC's
 * in it but the asking. Google's server first, Cloudflare's if that one does
 * not answer. RECOMP_NET_PUBLIC_IP=0 never asks.
 */
#ifndef XBOXRECOMP_NET_ADDR_H
#define XBOXRECOMP_NET_ADDR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This PC's IPv4 addresses another machine could reach -- not the loopback,
 * not a link-local one a network without DHCP hands out -- separated by ", ".
 * A VPN's (ZeroTier, Tailscale, Radmin) are among them. 0 when there are
 * none. */
int recomp_net_local_addrs(char *out, size_t n);

/* The address the internet sees this PC at, from STUN. Blocks for at most
 * about timeout_ms; 0 when nobody answered (offline, UDP blocked, or
 * RECOMP_NET_PUBLIC_IP=0). */
int recomp_net_public_addr(char *out, size_t n, unsigned timeout_ms);

/* An address no one on the internet can reach directly: a private network's
 * (10/8, 172.16/12, 192.168/16) or a carrier's shared space (100.64/10).
 * A router reporting one of these as its public address sits behind the
 * provider's own NAT, where no forwarded port helps. */
int recomp_net_addr_is_private(const char *ip);

#ifdef __cplusplus
}
#endif

#endif
