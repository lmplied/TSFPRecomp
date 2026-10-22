/*
 * recomp_invite.h -- invite codes: two games finding each other without a
 * forwarded port.
 *
 * A host's game makes a code (FP-XXXX-XXXX-XXXX) and the friend types it.
 * Each game has to learn where the other can reach it -- its addresses on
 * its own networks, and the address and port the internet sees, from STUN on
 * the very socket it will play on -- and those have to cross over before
 * either can send the other a thing. They cross on a public message relay:
 * ntfy.sh, a free and open-source publish-subscribe service (or any ntfy
 * server, RECOMP_NET_SIGNAL=https://...; RECOMP_NET_SIGNAL=0 for none). The
 * code names the channel and is its key: the topic is a hash of the code and
 * every message is AES-256-GCM sealed with another, so the relay holds an
 * unguessable name and ciphertext, and a message that was not sealed with
 * the code is not read.
 *
 * Then both send to every address the other gave at once, which opens each
 * one's router to the other (hole punching): through most home routers and
 * many providers' shared addresses, where a forwarded port is impossible.
 * hle_netplay.c does that part.
 */
#ifndef XBOXRECOMP_NET_INVITE_H
#define XBOXRECOMP_NET_INVITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_INVITE_CHARS 12          /* of the code, without the dashes */

/* A new code, "FP-XXXX-XXXX-XXXX": twelve of 31 letters and digits (none
 * that read alike), about 59 bits. 0 if the system's random numbers
 * failed. */
int recomp_invite_new(char *out, size_t n);

/* Whether `text` is a code -- in any case, with or without the dashes, the
 * spaces or the FP -- and if so, written out as recomp_invite_new writes
 * one. An address is not a code. */
int recomp_invite_parse(const char *text, char *code, size_t n);

/* A code's channel on the relay; NULL when the relay is turned off
 * (RECOMP_NET_SIGNAL=0) or the system has no AES. */
typedef struct RecompInvite RecompInvite;
RecompInvite *recomp_invite_open(const char *code);
void recomp_invite_close(RecompInvite *inv);

/* Sealed and posted: 1 once the relay has it. */
int recomp_invite_publish(RecompInvite *inv, const char *text);

/* The messages of the last `minutes`, each unsealed and handed to `got`
 * (oldest first); the number handed over, or -1 when the relay could not be
 * reached. */
int recomp_invite_poll(RecompInvite *inv, unsigned minutes,
                       void (*got)(void *ctx, const char *text), void *ctx);

/* Listening on a thread of its own: each message posted from now on,
 * unsealed, to `got` (on that thread) as it comes, until close. 0 when the
 * thread could not be started. */
int recomp_invite_listen(RecompInvite *inv, void (*got)(void *ctx, const char *text), void *ctx);

/* The relay's name, for a message to the player ("ntfy.sh"). */
const char *recomp_invite_service(void);

/* The address and port the internet sees `sock` at -- a UDP socket of the
 * caller's, bound -- from a STUN server. Other packets that arrive on it
 * meanwhile are dropped, so ask before anyone else knows the socket. 0 when
 * no server answered within about timeout_ms. */
int recomp_net_stun_on(uintptr_t sock, char *ip, size_t n, unsigned *port, unsigned timeout_ms);

/* What this connection does to a game's packets on their way out, from two
 * STUN servers asked from one socket:
 *   OPEN     this PC is on the internet itself: no router in the way
 *   GOOD     both saw the same address and port: the router keeps one
 *            mapping a socket, so an invite's direct link gets through
 *   STRICT   they saw different ports: a new mapping for each destination
 *            (symmetric NAT, common behind a provider's shared address, and
 *            where no port can be forwarded). A direct link then works only
 *            with an OPEN or GOOD connection on the other side.
 *   UNKNOWN  only one answered; OFFLINE neither */
enum { RECOMP_NAT_OFFLINE, RECOMP_NAT_UNKNOWN, RECOMP_NAT_OPEN, RECOMP_NAT_GOOD,
       RECOMP_NAT_STRICT };
typedef struct {
    int      kind;              /* RECOMP_NAT_* */
    char     ip[64];            /* the public address the first server saw */
    unsigned port;
    int      port_kept;         /* the public port is the socket's own */
} RecompNat;

/* On the caller's socket (bound, nobody else knowing it yet), or on one of
 * its own. Blocks for at most about timeout_ms. */
int recomp_net_nat_on(uintptr_t sock, RecompNat *out, unsigned timeout_ms);
int recomp_net_nat_check(RecompNat *out, unsigned timeout_ms);
const char *recomp_net_nat_name(int kind);       /* "open", "good", ... */

#ifdef __cplusplus
}
#endif

#endif
