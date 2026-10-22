/*
 * hle_netplay.c -- see hle_netplay.h.
 */
#include "hle_netplay.h"
#include "input_host.h"
#include "recomp_config.h"
#include "recomp_net_addr.h"        /* this PC's addresses, for the host to give out */
#include "recomp_invite.h"          /* invite codes: finding each other without a forwarded port */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32

#define _WINSOCK_DEPRECATED_NO_WARNINGS     /* inet_ntoa, for the log */

#define COBJMACROS                          /* natupnp.h's interfaces, from C */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <oleauto.h>
#include <natupnp.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

extern ptrdiff_t g_xbox_mem_offset;
extern void xbox_AtHostExit(void (*fn)(void));      /* kernel_bridge.c */
extern void xbox_HostExit(const char *why);

#define NET_MAGIC    0x314E5046u     /* "FPN1" */
#define NET_PROTOCOL 1u              /* a session's setup: both must speak the same */
#define RING         1024u           /* frames of controls kept, each side */
#define PER_PACKET   16u             /* frames of controls in a packet */
#define HASH_EVERY   60u
#define CHUNK_BYTES  1024u           /* of the saves, in one setup packet */
#define MAX_SAVE     (CHUNK_BYTES * 8u * CHUNK_BYTES)   /* 8 MB: what a NEED's bitmap covers */
#define BURST        48u             /* chunks sent for one NEED */
#define ALIVE_MS     250u            /* PKT_ALIVE's period */

enum {
    PKT_HELLO = 1, PKT_WELCOME = 2, PKT_INPUTS = 3,
    PKT_ALIVE = 4,      /* every quarter second, so a silence means the other side is gone */
    PKT_BYE = 5,        /* this side's game is closing */
    /* A session's setup (hle_net_session_begin), before either game starts. */
    PKT_JOIN = 11,      /* a: protocol, b: build, c: the game's files */
    PKT_REFUSE,         /* a: why (REFUSE_*), b and c: the host's build and game */
    PKT_OFFER,          /* a: bytes of saves, b: their hash; data: the settings */
    PKT_NEED,           /* data: a bit for each chunk of the saves already here */
    PKT_CHUNK,          /* a: which chunk; data: its bytes */
    PKT_READY,          /* a: the round trip, in ms */
    PKT_GO,             /* a: the input delay both use, in frames */
    PKT_PING,           /* a and b: echoed in a PONG */
    PKT_PONG,
    PKT_PUNCH = 20      /* nothing: sent to open this side's router to the other */
};

enum { REFUSE_BUILD = 1, REFUSE_GAME = 2, REFUSE_FULL = 3 };

/* What the host's settings decide for both machines, in an OFFER's data. */
enum { SET_SKIP_INTRO, SET_WIDESCREEN, SET_COUNT };

#pragma pack(push, 1)
typedef struct {
    uint16_t buttons;
    uint8_t  analog[RECOMP_INPUT_ANALOG_BUTTON_COUNT];
    int16_t  lx, ly, rx, ry;
    float    aim_x, aim_y;           /* the mouse's turn, in the title's units */
} NetInput;

typedef struct {
    uint32_t magic;
    uint8_t  type, player;
    uint16_t count;
    uint32_t first;                  /* the frame of inputs[0] */
    uint32_t ack;                    /* every frame of the receiver's up to here has arrived */
    uint32_t hash_frame, hash;       /* the sender's latest state hash */
    NetInput inputs[PER_PACKET];
} NetPacket;

typedef struct {
    uint32_t magic;
    uint8_t  type, flags;
    uint16_t len;                    /* bytes of data */
    uint32_t a, b, c;                /* by type, above */
    uint8_t  data[CHUNK_BYTES];
} NetSetup;
#pragma pack(pop)

static struct {
    int              configured, active, host, delay;
    int              supported;      /* the title starts sessions (hle_net_session_begin) */
    unsigned         input_port;     /* the local controls' port: 1 for a second copy on one PC */
    unsigned         rtt;            /* the round trip measured as the session began, ms */
    unsigned         me;             /* this machine's game port */
    unsigned         port;           /* the host's UDP port */
    SOCKET           sock;
    struct sockaddr_in peer;
    int              have_peer, connected;
    CRITICAL_SECTION lock;
    HANDLE           arrived;        /* set when something comes in */
    NetInput         mine[RING], theirs[RING];
    uint32_t         mine_frame[RING], theirs_frame[RING];   /* which frame each slot holds, +1 */
    uint32_t         their_ack;      /* of ours, everything up to here arrived */
    uint32_t         their_contig;   /* of theirs, everything up to here has arrived */
    RecompInputGamepad now[2];       /* the controls of the frame running, by game port */
    float            now_aim[2][2];  /* and the mouse's turn */
    float            pending_aim[2]; /* this machine's turn since its last controls went out */
    uint32_t         hash_va, hash_size;
    uint32_t         my_hash[RING / HASH_EVERY + 1], my_hash_frame[RING / HASH_EVERY + 1];
    uint32_t         their_hash_frame, their_hash;
    int              desynced;
    ULONGLONG        slow_at;        /* when a frame last waited long for the other side */
    volatile ULONGLONG heard_at;     /* when the other side last sent anything */
    volatile int     peer_left;      /* the other side said PKT_BYE */
} g_net;

static void to_net(const RecompInputGamepad *g, NetInput *n)
{
    memset(n, 0, sizeof *n);
    n->buttons = g->buttons;
    memcpy(n->analog, g->analog_buttons, sizeof n->analog);
    n->lx = g->thumb_lx; n->ly = g->thumb_ly; n->rx = g->thumb_rx; n->ry = g->thumb_ry;
}

static void from_net(const NetInput *n, RecompInputGamepad *g)
{
    memset(g, 0, sizeof *g);
    g->buttons = n->buttons;
    memcpy(g->analog_buttons, n->analog, sizeof n->analog);
    g->thumb_lx = n->lx; g->thumb_ly = n->ly; g->thumb_rx = n->rx; g->thumb_ry = n->ry;
}

static uint32_t fnv(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811C9DC5u;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Milliseconds, finer than GetTickCount's sixteen: a LAN's round trip is one. */
static uint32_t now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;

    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint32_t)(t.QuadPart * 1000 / f.QuadPart);
}

/* FNV-1a over the game state range, as hle_lockstep hashes a chunk. */
static uint32_t state_hash(void)
{
    const uint64_t *p;
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t i, words;

    if (!g_net.hash_size)
        return 0;
    p = (const uint64_t *)((uintptr_t)g_net.hash_va + g_xbox_mem_offset);
    words = g_net.hash_size / 8u;
    for (i = 0; i < words; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static void check_hashes(void)
{
    unsigned k = (g_net.their_hash_frame / HASH_EVERY) % (RING / HASH_EVERY + 1);

    if (g_net.desynced || !g_net.their_hash_frame || g_net.my_hash_frame[k] != g_net.their_hash_frame)
        return;
    if (g_net.my_hash[k] != g_net.their_hash) {
        g_net.desynced = 1;
        fprintf(stderr, "[NET] DESYNC: the two games differ at frame %u (%08X here, %08X there)\n",
                g_net.their_hash_frame, g_net.my_hash[k], g_net.their_hash);
        fflush(stderr);
    }
}

static int from_peer(const struct sockaddr_in *from)
{
    return g_net.have_peer && from->sin_addr.s_addr == g_net.peer.sin_addr.s_addr &&
           from->sin_port == g_net.peer.sin_port;
}

static void send_packet(int type, uint32_t upto)
{
    NetPacket p;
    uint32_t first, f;
    unsigned k = 0;

    if (!g_net.have_peer)
        return;
    memset(&p, 0, sizeof p);
    p.magic = NET_MAGIC;
    p.type = (uint8_t)type;
    p.player = (uint8_t)g_net.me;
    EnterCriticalSection(&g_net.lock);
    p.ack = g_net.their_contig;
    {
        /* the latest hash this side has */
        unsigned i, best = 0;
        for (i = 0; i < RING / HASH_EVERY + 1; i++)
            if (g_net.my_hash_frame[i] > g_net.my_hash_frame[best])
                best = i;
        p.hash_frame = g_net.my_hash_frame[best];
        p.hash = g_net.my_hash[best];
    }
    if (type == PKT_INPUTS && upto) {
        first = upto + 1u > PER_PACKET ? upto + 1u - PER_PACKET : 1u;
        if (first <= g_net.their_ack)
            first = g_net.their_ack + 1u;
        if (first > upto)
            first = upto;                    /* always the newest, as a heartbeat */
        p.first = first;
        for (f = first; f <= upto && k < PER_PACKET; f++) {
            if (g_net.mine_frame[f % RING] == f + 1u)
                p.inputs[k] = g_net.mine[f % RING];
            k++;
        }
        p.count = (uint16_t)k;
    }
    LeaveCriticalSection(&g_net.lock);
    sendto(g_net.sock, (const char *)&p, (int)(offsetof(NetPacket, inputs) + k * sizeof(NetInput)),
           0, (const struct sockaddr *)&g_net.peer, sizeof g_net.peer);
}

static void send_setup(const struct sockaddr_in *to, int type, uint32_t a, uint32_t b,
                       uint32_t c, const void *data, unsigned len)
{
    NetSetup s;

    memset(&s, 0, offsetof(NetSetup, data));
    s.magic = NET_MAGIC;
    s.type = (uint8_t)type;
    s.len = (uint16_t)len;
    s.a = a;
    s.b = b;
    s.c = c;
    if (len)
        memcpy(s.data, data, len);
    sendto(g_net.sock, (const char *)&s, (int)(offsetof(NetSetup, data) + len), 0,
           (const struct sockaddr *)to, sizeof *to);
}

/* A setup packet once the games are running: the joiner's READY again (the
 * host's GO was lost), a ping, or someone else trying to join. */
static void setup_late(const NetSetup *s, const struct sockaddr_in *from)
{
    if (!from_peer(from)) {
        if (g_net.host && s->type == PKT_JOIN)
            send_setup(from, PKT_REFUSE, REFUSE_FULL, 0, 0, NULL, 0);
        return;
    }
    if (g_net.host && s->type == PKT_READY)
        send_setup(from, PKT_GO, (uint32_t)g_net.delay, 0, 0, NULL, 0);
    else if (s->type == PKT_PING)
        send_setup(from, PKT_PONG, s->a, s->b, 0, NULL, 0);
}

/* While the game is busy -- a level loading holds its frames for seconds --
 * this side still says it is there, from the receiving thread, which never
 * waits on the game. */
static void send_alive(void)
{
    static ULONGLONG sent_at;
    ULONGLONG now = GetTickCount64();

    if (g_net.have_peer && g_net.connected && now - sent_at >= ALIVE_MS) {
        sent_at = now;
        send_packet(PKT_ALIVE, 0);
    }
}

static DWORD WINAPI receive_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        union { NetPacket p; NetSetup s; char raw[1500]; } u;
        NetPacket *p = &u.p;
        struct sockaddr_in from;
        int fromlen = sizeof from, got;
        fd_set set;
        struct timeval tv;
        uint32_t i;

        send_alive();
        FD_ZERO(&set);
        FD_SET(g_net.sock, &set);
        tv.tv_sec = 0;
        tv.tv_usec = (long)ALIVE_MS * 1000 / 2;
        if (select(0, &set, NULL, NULL, &tv) <= 0)
            continue;
        got = recvfrom(g_net.sock, u.raw, sizeof u.raw, 0, (struct sockaddr *)&from, &fromlen);
        if (got < (int)offsetof(NetSetup, data) || p->magic != NET_MAGIC)
            continue;
        if (from_peer(&from))
            g_net.heard_at = GetTickCount64();
        if (p->type >= PKT_JOIN) {
            setup_late(&u.s, &from);
            continue;
        }
        if (got < (int)offsetof(NetPacket, inputs))
            continue;
        if (g_net.have_peer && !from_peer(&from))
            continue;                        /* not the other player */
        if (p->type == PKT_BYE) {
            if (!g_net.peer_left) {
                g_net.peer_left = 1;
                fprintf(stderr, "[NET] the other player has left the game\n");
                fflush(stderr);
            }
            SetEvent(g_net.arrived);
            continue;
        }
        EnterCriticalSection(&g_net.lock);
        if (g_net.host && !g_net.have_peer && p->type == PKT_HELLO) {
            g_net.peer = from;
            g_net.have_peer = 1;
            fprintf(stderr, "[NET] player 2 joined from %s:%u\n", inet_ntoa(from.sin_addr),
                    ntohs(from.sin_port));
            fflush(stderr);
        }
        if (p->type == PKT_HELLO || p->type == PKT_WELCOME || p->type == PKT_INPUTS ||
            p->type == PKT_ALIVE) {
            if (!g_net.connected) {
                g_net.connected = 1;
                if (!g_net.host) {
                    fprintf(stderr, "[NET] joined the host as player 2\n");
                    fflush(stderr);
                }
            }
        }
        if (p->ack > g_net.their_ack)
            g_net.their_ack = p->ack;
        if (p->hash_frame && p->hash_frame != g_net.their_hash_frame) {
            g_net.their_hash_frame = p->hash_frame;
            g_net.their_hash = p->hash;
            check_hashes();
        }
        if (p->type == PKT_INPUTS) {
            uint32_t count = p->count;
            if ((size_t)got < offsetof(NetPacket, inputs) + count * sizeof(NetInput))
                count = 0;
            for (i = 0; i < count && i < PER_PACKET; i++) {
                uint32_t f = p->first + i;
                if (f && g_net.theirs_frame[f % RING] != f + 1u && f > g_net.their_contig) {
                    g_net.theirs[f % RING] = p->inputs[i];
                    g_net.theirs_frame[f % RING] = f + 1u;
                }
            }
            while (g_net.theirs_frame[(g_net.their_contig + 1u) % RING] == g_net.their_contig + 2u)
                g_net.their_contig++;
        }
        LeaveCriticalSection(&g_net.lock);
        if (p->type == PKT_HELLO && g_net.host)
            send_packet(PKT_WELCOME, 0);
        SetEvent(g_net.arrived);
    }
}

/* -------------------------------------------------------------- invites */

/* An invite code (recomp_invite.h): the channel on the relay, and where
 * each game said it can be reached -- its addresses on its networks and the
 * one STUN saw, each with the port it plays on -- as "FPCOOP1 H|J <build>
 * <ip:port>,<ip:port>,...". */
#define MAX_CANDS 8

static struct {
    char          code[24];
    RecompInvite *inv;
    struct sockaddr_in peer[MAX_CANDS];   /* where the other game can be reached */
    volatile LONG npeer;
    char          mine[400];
    int           published;
    int           my_nat, peer_nat;       /* RECOMP_NAT_*: this connection's, and the other's */
} g_invite;

static int parse_cands(const char *list, struct sockaddr_in *out, int max)
{
    char copy[400], *p, *ctx = NULL;
    int n = 0;

    snprintf(copy, sizeof copy, "%s", list);
    for (p = strtok_s(copy, ",", &ctx); p && n < max; p = strtok_s(NULL, ",", &ctx)) {
        char ip[64];
        unsigned port;

        if (sscanf(p, "%63[0-9.]:%u", ip, &port) != 2 || !port || port > 65535)
            continue;
        memset(&out[n], 0, sizeof out[n]);
        out[n].sin_family = AF_INET;
        out[n].sin_port = htons((u_short)port);
        if (inet_pton(AF_INET, ip, &out[n].sin_addr) == 1)
            n++;
    }
    return n;
}

/* Where this game can be reached: each of this PC's addresses with the port
 * it plays on, and what STUN sees of that same socket. Asked before anyone
 * else knows the socket: STUN's answer is read from it. */
static void gather_mine(void)
{
    struct sockaddr_in a;
    int len = sizeof a;
    unsigned port = 0, pub_port = 0;
    char lan[256], pub[64], *p, *ctx = NULL;
    size_t n = 0;

    if (getsockname(g_net.sock, (struct sockaddr *)&a, &len) == 0)
        port = ntohs(a.sin_port);
    recomp_net_local_addrs(lan, sizeof lan);
    /* RECOMP_NET_INVITE_PUBLIC_ONLY=1: only what STUN sees, as if the two
     * were on different networks -- a test, on one, of the way through the
     * router (it has to send a packet back to its own public address). */
    {
        const char *e = getenv("RECOMP_NET_INVITE_PUBLIC_ONLY");
        if (e && *e && strcmp(e, "0") != 0)
            lan[0] = 0;
    }
    for (p = strtok_s(lan, ", ", &ctx); p && n < sizeof g_invite.mine - 32;
         p = strtok_s(NULL, ", ", &ctx))
        n += (size_t)snprintf(g_invite.mine + n, sizeof g_invite.mine - n, "%s%s:%u",
                              n ? "," : "", p, port);
    {
        /* Two STUN servers from this socket: the address to give out, and
         * what kind of connection it is (recomp_invite.h). */
        RecompNat nat;
        if (recomp_net_nat_on((uintptr_t)g_net.sock, &nat, 3000) && nat.ip[0] &&
            n < sizeof g_invite.mine - 32)
            n += (size_t)snprintf(g_invite.mine + n, sizeof g_invite.mine - n, "%s%s:%u",
                                  n ? "," : "", nat.ip, nat.port);
        g_invite.my_nat = nat.kind;
        (void)pub; (void)pub_port;
    }
    fprintf(stderr, "[NET] invite: this game can be reached at %s; the connection is %s\n",
            g_invite.mine[0] ? g_invite.mine : "(nowhere found)",
            recomp_net_nat_name(g_invite.my_nat));
    fflush(stderr);
}

/* A message on the invite's channel: the other game's addresses. The
 * latest one stands (an earlier game with the same code is history). */
static void invite_heard(void *ctx, const char *text)
{
    struct sockaddr_in c[MAX_CANDS];
    char role, list[400], nat[16];
    unsigned build;
    int n, kind;

    (void)ctx;
    nat[0] = 0;
    if (sscanf(text, "FPCOOP1 %c %x %399s nat=%15s", &role, &build, list, nat) < 3 ||
        role != (g_net.host ? 'J' : 'H') || (n = parse_cands(list, c, MAX_CANDS)) == 0)
        return;
    for (kind = RECOMP_NAT_STRICT; kind > RECOMP_NAT_OFFLINE; kind--)
        if (strcmp(nat, recomp_net_nat_name(kind)) == 0)
            break;
    g_invite.peer_nat = kind;
    EnterCriticalSection(&g_net.lock);
    memcpy(g_invite.peer, c, (size_t)n * sizeof c[0]);
    InterlockedExchange(&g_invite.npeer, n);
    LeaveCriticalSection(&g_net.lock);
    fprintf(stderr, "[NET] invite: the %s can be reached at %s\n",
            g_net.host ? "joining game" : "host's game", list);
    fflush(stderr);
}

static int invite_publish(uint32_t build)
{
    char text[480];

    snprintf(text, sizeof text, "FPCOOP1 %c %08X %s nat=%s", g_net.host ? 'H' : 'J', build,
             g_invite.mine[0] ? g_invite.mine : "0.0.0.0:0", recomp_net_nat_name(g_invite.my_nat));
    g_invite.published = recomp_invite_publish(g_invite.inv, text);
    return g_invite.published;
}

static void invite_close(void)
{
    recomp_invite_close(g_invite.inv);
    g_invite.inv = NULL;
}

/* The other game's addresses, while the first packet has not come through:
 * one of each, which opens this side's router to the replies. */
static void invite_punch(int type, uint32_t a, uint32_t b, uint32_t c)
{
    struct sockaddr_in to[MAX_CANDS];
    int i, n;

    EnterCriticalSection(&g_net.lock);
    n = (int)g_invite.npeer;
    memcpy(to, g_invite.peer, (size_t)n * sizeof to[0]);
    LeaveCriticalSection(&g_net.lock);
    for (i = 0; i < n; i++)
        send_setup(&to[i], type, a, b, c, NULL, 0);
}

/* A packet from one of the other game's addresses, before either knew which
 * would get through: that one is the way. */
static int invite_adopt(const struct sockaddr_in *from)
{
    int i, n = (int)g_invite.npeer;

    for (i = 0; i < n; i++)
        if (g_invite.peer[i].sin_addr.s_addr == from->sin_addr.s_addr) {
            char ip[64];
            g_net.peer = *from;
            g_net.have_peer = 1;
            inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
            fprintf(stderr, "[NET] invite: through to %s:%u\n", ip, ntohs(from->sin_port));
            fflush(stderr);
            return 1;
        }
    return 0;
}

static int socket_bound(SOCKET s)
{
    struct sockaddr_in a;
    int len = sizeof a;

    return getsockname(s, (struct sockaddr *)&a, &len) == 0;
}

/* RECOMP_NET read and its socket open; 0, with the reason on stderr, when
 * either cannot be. */
static int open_socket(const char *spec)
{
    const char *d = getenv("RECOMP_NET_DELAY");
    WSADATA wsa;
    struct sockaddr_in bind_to;
    char host[128];
    unsigned port = 0;
    BOOL no_reset = FALSE;
    DWORD unused;

    g_net.delay = d ? atoi(d) : 3;
    if (g_net.delay < 1) g_net.delay = 1;
    if (g_net.delay > 30) g_net.delay = 30;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;
    memset(&bind_to, 0, sizeof bind_to);
    bind_to.sin_family = AF_INET;
    if (strncmp(spec, "host:", 5) == 0) {
        const char *code = strchr(spec + 5, ':');
        g_net.host = 1;
        g_net.me = 0;
        port = (unsigned)atoi(spec + 5);
        bind_to.sin_port = htons((u_short)port);
        if (code && !recomp_invite_parse(code + 1, g_invite.code, sizeof g_invite.code))
            g_invite.code[0] = 0;
    } else if (strncmp(spec, "invite:", 7) == 0 &&
               recomp_invite_parse(spec + 7, g_invite.code, sizeof g_invite.code)) {
        g_net.host = 0;                 /* the host's addresses come from the invite */
        g_net.me = 1;
    } else if (strncmp(spec, "join:", 5) == 0 &&
               sscanf(spec + 5, "%127[^:]:%u", host, &port) == 2) {
        struct addrinfo hints, *res = NULL;
        g_net.host = 0;
        g_net.me = 1;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, NULL, &hints, &res) || !res) {
            fprintf(stderr, "[NET] cannot find the host '%s'\n", host);
            fflush(stderr);
            return 0;
        }
        g_net.peer = *(struct sockaddr_in *)res->ai_addr;
        g_net.peer.sin_port = htons((u_short)port);
        g_net.have_peer = 1;
        freeaddrinfo(res);
    } else {
        fprintf(stderr, "[NET] RECOMP_NET=%s: want host:<port>[:<invite code>], "
                "join:<address>:<port> or invite:<code>\n", spec);
        fflush(stderr);
        return 0;
    }
    g_net.port = port;
    g_net.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    /* A game restarting into co-op (hle_net_restart) may still be letting go
     * of the port: a few seconds' grace. */
    {
        int tries;
        for (tries = 0; g_net.sock != INVALID_SOCKET && tries < 30 &&
                        bind(g_net.sock, (const struct sockaddr *)&bind_to, sizeof bind_to) != 0 &&
                        WSAGetLastError() == WSAEADDRINUSE; tries++)
            Sleep(100);
    }
    if (g_net.sock == INVALID_SOCKET || !socket_bound(g_net.sock)) {
        fprintf(stderr, "[NET] cannot open UDP port %u (error %d)\n", g_net.host ? port : 0,
                WSAGetLastError());
        fflush(stderr);
        return 0;
    }
    /* A packet to a port nobody has opened yet -- the host's game still
     * starting -- comes back as an error on the next receive, on Windows.
     * Here that is only waiting. */
    WSAIoctl(g_net.sock, SIO_UDP_CONNRESET, &no_reset, sizeof no_reset, NULL, 0, &unused,
             NULL, NULL);
    InitializeCriticalSection(&g_net.lock);
    g_net.arrived = CreateEventW(NULL, FALSE, FALSE, NULL);
    return 1;
}

/* ---------------------------------------------- the host's port, by UPnP */

/* A host behind a home router is reachable from the internet only on a
 * port the router forwards. Most routers will forward one when asked
 * (UPnP), which is what this does, on its own thread while the host waits
 * -- finding the router can take seconds -- and it learns the public
 * address to give out on the way. Given back when the game closes, and a
 * forward this PC left before (a game that did not close cleanly) is taken
 * back first; another PC's is never touched. RECOMP_NET_UPNP=0 leaves the
 * router alone. Windows' own UPnP client (natupnp.h). */
enum { UPNP_OFF, UPNP_TRYING, UPNP_OPEN, UPNP_NONE, UPNP_TAKEN };
static volatile LONG g_upnp;
static char g_public[64];

static const GUID k_clsid_upnp_nat = {
    0xAE1E00AA, 0x3FD5, 0x403C, { 0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1 } };
static const GUID k_iid_upnp_nat = {
    0xB171C812, 0xCC76, 0x485A, { 0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99 } };

/* The address this PC reaches the internet from: the source a packet to a
 * public address would have. Nothing is sent; a UDP connect only routes. */
static int outward_addr(char *out, size_t n)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in to, me;
    int len = sizeof me, ok = 0;

    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    if (s != INVALID_SOCKET && connect(s, (const struct sockaddr *)&to, sizeof to) == 0 &&
        getsockname(s, (struct sockaddr *)&me, &len) == 0 &&
        inet_ntop(AF_INET, &me.sin_addr, out, n))
        ok = 1;
    if (s != INVALID_SOCKET)
        closesocket(s);
    return ok;
}

static IStaticPortMappingCollection *upnp_maps(IUPnPNAT **nat)
{
    IStaticPortMappingCollection *maps = NULL;

    *nat = NULL;
    if (FAILED(CoCreateInstance(&k_clsid_upnp_nat, NULL, CLSCTX_INPROC_SERVER, &k_iid_upnp_nat,
                                (void **)nat)) || !*nat)
        return NULL;
    if (FAILED(IUPnPNAT_get_StaticPortMappingCollection(*nat, &maps)))
        maps = NULL;
    return maps;                     /* NULL too when no router answers */
}

static DWORD WINAPI upnp_open_thread(LPVOID unused)
{
    IUPnPNAT *nat;
    IStaticPortMappingCollection *maps;
    IStaticPortMapping *map = NULL;
    BSTR udp = SysAllocString(L"UDP"), what = SysAllocString(L"TimeSplitters FP online"), client;
    wchar_t wme[64];
    char me[64];
    LONG result = UPNP_NONE;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    (void)unused;
    if (!outward_addr(me, sizeof me) || !(maps = upnp_maps(&nat))) {
        if (nat)
            IUPnPNAT_Release(nat);
    } else {
        MultiByteToWideChar(CP_ACP, 0, me, -1, wme, 64);
        client = SysAllocString(wme);
        if (SUCCEEDED(IStaticPortMappingCollection_get_Item(maps, (long)g_net.port, udp, &map)) &&
            map) {
            BSTR who = NULL;
            int ours = SUCCEEDED(IStaticPortMapping_get_InternalClient(map, &who)) && who &&
                       wcscmp(who, wme) == 0;
            SysFreeString(who);
            IStaticPortMapping_Release(map);
            map = NULL;
            if (ours)
                IStaticPortMappingCollection_Remove(maps, (long)g_net.port, udp);
            else
                result = UPNP_TAKEN;
        }
        if (result != UPNP_TAKEN &&
            SUCCEEDED(IStaticPortMappingCollection_Add(maps, (long)g_net.port, udp,
                                                       (long)g_net.port, client, VARIANT_TRUE,
                                                       what, &map)) && map) {
            BSTR ext = NULL;
            if (SUCCEEDED(IStaticPortMapping_get_ExternalIPAddress(map, &ext)) && ext)
                WideCharToMultiByte(CP_ACP, 0, ext, -1, g_public, sizeof g_public, NULL, NULL);
            SysFreeString(ext);
            IStaticPortMapping_Release(map);
            result = UPNP_OPEN;
        }
        SysFreeString(client);
        IStaticPortMappingCollection_Release(maps);
        IUPnPNAT_Release(nat);
    }
    SysFreeString(udp);
    SysFreeString(what);
    fprintf(stderr, "[NET] UPnP: %s%s\n",
            result == UPNP_OPEN ? "the router forwards the port; public address "
            : result == UPNP_TAKEN ? "the router already forwards the port to another PC"
            : "no router answered, or it would not forward the port", result == UPNP_OPEN ? g_public : "");
    fflush(stderr);
    InterlockedExchange(&g_upnp, result);
    if (SUCCEEDED(co))
        CoUninitialize();
    return 0;
}

static void upnp_open(void)
{
    const char *e = getenv("RECOMP_NET_UPNP");

    if (e && *e && strcmp(e, "0") == 0)
        return;
    g_upnp = UPNP_TRYING;
    CloseHandle(CreateThread(NULL, 0, upnp_open_thread, NULL, 0, NULL));
}

static DWORD WINAPI upnp_close_thread(LPVOID unused)
{
    IUPnPNAT *nat;
    IStaticPortMappingCollection *maps;
    BSTR udp = SysAllocString(L"UDP");
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    (void)unused;
    if ((maps = upnp_maps(&nat)) != NULL) {
        IStaticPortMappingCollection_Remove(maps, (long)g_net.port, udp);
        IStaticPortMappingCollection_Release(maps);
    }
    if (nat)
        IUPnPNAT_Release(nat);
    SysFreeString(udp);
    if (SUCCEEDED(co))
        CoUninitialize();
    return 0;
}

/* The forward given back, waiting a little for the router: the process is
 * on its way out. */
static void upnp_close(void)
{
    HANDLE t;

    if (InterlockedExchange(&g_upnp, UPNP_OFF) != UPNP_OPEN)
        return;
    t = CreateThread(NULL, 0, upnp_close_thread, NULL, 0, NULL);
    if (t) {
        WaitForSingleObject(t, 3000);
        CloseHandle(t);
    }
}

/* The game closing (xbox_HostExit, or Quit in the wait's window): the other
 * side is told rather than left to find out by the silence. */
static void net_goodbye(void)
{
    static LONG said;
    int i;

    if (InterlockedExchange(&said, 1))
        return;
    if (g_net.have_peer)
        for (i = 0; i < 3; i++)
            send_packet(PKT_BYE, 0);
    upnp_close();
}

static void start_lockstep(void)
{
    /* The frames before the delay has filled are an untouched pad on both
     * sides and never sent: count them as arrived. */
    g_net.their_contig = g_net.their_ack = (uint32_t)g_net.delay + 1u;
    CreateThread(NULL, 0, receive_thread, NULL, 0, NULL);
    xbox_AtHostExit(net_goodbye);
    g_net.active = 1;
    fprintf(stderr, "[NET] lockstep %s, player %u, %d frame(s) of input delay\n",
            g_net.host ? "host" : "guest", g_net.me + 1, g_net.delay);
    fflush(stderr);
}

/* Without hle_net_session_begin: the socket on first use, and the other
 * side met at the first frame (wait_connected). */
static void configure(void)
{
    const char *spec = getenv("RECOMP_NET");

    g_net.configured = 1;
    if (spec && *spec && open_socket(spec))
        start_lockstep();
}

/* ------------------------------------------------------ a session's setup */

/* A small window of its own while the two machines meet, before the game's
 * window exists: what is happening, and a way out. */
static HWND g_status, g_status_text, g_status_button, g_status_copy_button;
static volatile int g_status_cancel;
static int  g_status_with_copy;         /* the host's window: a Copy button too */
static char g_status_copy[96];          /* what it copies: the address to give out */

#define ID_COPY 101

void hle_net_copy_text(const char *text)
{
    size_t n = strlen(text) + 1;
    HGLOBAL h;

    if (!OpenClipboard(NULL))
        return;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) {
        memcpy(GlobalLock(h), text, n);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_TEXT, h))
            GlobalFree(h);
    }
    CloseClipboard();
}

static LRESULT CALLBACK status_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if ((m == WM_COMMAND && LOWORD(wp) == IDCANCEL) || m == WM_CLOSE) {
        g_status_cancel = 1;
        return 0;
    }
    if (m == WM_COMMAND && LOWORD(wp) == ID_COPY && g_status_copy[0]) {
        hle_net_copy_text(g_status_copy);
        SetWindowTextW(g_status_copy_button, L"Copied");
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* `quiet`: over the game while it plays, so not taking the focus from it --
 * as for RECOMP_WINDOW_BACKGROUND (hle_d3d8.c), a run a script drives. */
static void status_show(const char *text, const wchar_t *button, int quiet)
{
    if (!g_status) {
        const int W = 560, H = 260;
        const char *bg = getenv("RECOMP_WINDOW_BACKGROUND");
        int background = bg && *bg && strcmp(bg, "0") != 0;
        WNDCLASSW wc;
        RECT r = { 0, 0, W, H };
        HINSTANCE self = GetModuleHandleW(NULL);
        HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HWND cancel;

        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = status_proc;
        wc.hInstance = self;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"RecompNetStatus";
        RegisterClassW(&wc);
        AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
        g_status_cancel = 0;
        g_status = CreateWindowExW(background ? WS_EX_NOACTIVATE
                                   : quiet ? WS_EX_TOPMOST | WS_EX_NOACTIVATE : WS_EX_TOPMOST,
                                   wc.lpszClassName, L"Co-op", WS_CAPTION | WS_SYSMENU,
                                   (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
                                   (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2,
                                   r.right - r.left, r.bottom - r.top, NULL, NULL, self, NULL);
        g_status_text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                        20, 18, W - 40, H - 70, g_status, NULL, self, NULL);
        cancel = CreateWindowExW(0, L"BUTTON", button, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 W - 116, H - 44, 96, 30, g_status, (HMENU)(INT_PTR)IDCANCEL,
                                 self, NULL);
        SendMessageW(g_status_text, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
        g_status_button = cancel;
        if (g_status_with_copy) {
            g_status_copy_button = CreateWindowExW(0, L"BUTTON",
                                                   g_invite.code[0] ? L"Copy code" : L"Copy address",
                                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                   W - 248, H - 44, 120, 30, g_status,
                                                   (HMENU)(INT_PTR)ID_COPY, self, NULL);
            SendMessageW(g_status_copy_button, WM_SETFONT, (WPARAM)font, TRUE);
        }
        if (background) {
            ShowWindow(g_status, SW_SHOWNOACTIVATE);
            SetWindowPos(g_status, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        } else {
            ShowWindow(g_status, quiet ? SW_SHOWNOACTIVATE : SW_SHOW);
        }
    }
    SetWindowTextA(g_status_text, text);
}

/* Before either game starts: the setup's window. */
static void status(const char *text)
{
    status_show(text, L"Cancel", 0);
}

/* The window answered; 0 once the player has cancelled. */
static int status_pump(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !g_status_cancel;
}

static void status_close(void)
{
    if (g_status)
        DestroyWindow(g_status);
    g_status = g_status_text = g_status_button = g_status_copy_button = NULL;
    g_status_with_copy = 0;
    status_pump();
}

static int setup_fail(const char *text)
{
    fprintf(stderr, "[NET] %s\n", text);
    fflush(stderr);
    MessageBoxA(g_status, text, "Co-op", MB_OK | MB_ICONERROR | MB_TOPMOST);
    status_close();
    return 0;
}

/* One setup packet, or 0 after `ms` with none. */
static int setup_recv(NetSetup *s, struct sockaddr_in *from, unsigned ms)
{
    fd_set set;
    struct timeval tv;
    int fromlen = sizeof *from, got;

    FD_ZERO(&set);
    FD_SET(g_net.sock, &set);
    tv.tv_sec = (long)(ms / 1000u);
    tv.tv_usec = (long)(ms % 1000u) * 1000;
    if (select(0, &set, NULL, NULL, &tv) <= 0)
        return 0;
    got = recvfrom(g_net.sock, (char *)s, sizeof *s, 0, (struct sockaddr *)from, &fromlen);
    if (got < (int)offsetof(NetSetup, data) || s->magic != NET_MAGIC || s->type < PKT_JOIN)
        return 0;
    if (s->len > CHUNK_BYTES || offsetof(NetSetup, data) + s->len > (size_t)got)
        s->len = 0;
    return 1;
}

/* The saves both games start from: the host's, sent to the joiner, which
 * keeps them in a folder of its own (`online` in its save folder) so its own
 * are never touched. As kernel_path.c lays the save folder out: the title's
 * UDATA\<title id> and TDATA\<title id>, in RECOMP_SAVE_DIR or the per-user
 * default. */
typedef struct {
    uint8_t *p;
    size_t   n, cap;
    int      over;                   /* past MAX_SAVE, or a file unreadable */
} Blob;

static void blob_put(Blob *b, const void *d, size_t n)
{
    if (b->over || b->n + n > MAX_SAVE) {
        b->over = 1;
        return;
    }
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2u : 65536u;
        uint8_t *q;
        while (cap < b->n + n)
            cap *= 2u;
        q = (uint8_t *)realloc(b->p, cap);
        if (!q) {
            b->over = 1;
            return;
        }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void save_base(wchar_t *out, size_t n)
{
    const char *env = getenv("RECOMP_SAVE_DIR");
    wchar_t base[MAX_PATH];

    if (env && *env) {
        MultiByteToWideChar(CP_UTF8, 0, env, -1, out, (int)n);
    } else if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        swprintf(out, n, L"%s\\xboxrecomp", base);
    } else {
        GetCurrentDirectoryW((DWORD)n, out);
        wcscat_s(out, n, L"\\SaveData");
    }
}

static const wchar_t *const k_roots[2] = { L"UDATA", L"TDATA" };

/* Every folder and file under `dir`, as entries: the root (k_roots), the
 * path under it in UTF-8, the size (all ones for a folder), the bytes. */
static void blob_tree(Blob *b, uint8_t root, const wchar_t *dir, const char *rel)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    swprintf(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char name[MAX_PATH * 3], sub[1024];
        uint16_t len;
        uint32_t size;

        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
        snprintf(sub, sizeof sub, rel[0] ? "%s\\%s" : "%s%s", rel, name);
        swprintf(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        len = (uint16_t)strlen(sub);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            size = 0xFFFFFFFFu;
            blob_put(b, &root, 1);
            blob_put(b, &len, 2);
            blob_put(b, sub, len);
            blob_put(b, &size, 4);
            blob_tree(b, root, path, sub);
        } else {
            HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            LARGE_INTEGER sz;
            uint8_t *data;
            DWORD got = 0;

            if (f == INVALID_HANDLE_VALUE) {
                b->over = 1;
                continue;
            }
            if (!GetFileSizeEx(f, &sz) || sz.QuadPart > MAX_SAVE) {
                CloseHandle(f);
                b->over = 1;
                continue;
            }
            size = (uint32_t)sz.QuadPart;
            data = (uint8_t *)malloc(size ? size : 1u);
            if (!data || (size && (!ReadFile(f, data, size, &got, NULL) || got != size))) {
                free(data);
                CloseHandle(f);
                b->over = 1;
                continue;
            }
            CloseHandle(f);
            blob_put(b, &root, 1);
            blob_put(b, &len, 2);
            blob_put(b, sub, len);
            blob_put(b, &size, 4);
            blob_put(b, data, size);
            free(data);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* The host's saves for this title. A title folder still only in the game
 * folder is the one kernel_path.c's migration is about to copy across, so
 * that is the one sent. */
static void blob_saves(Blob *b, const char *game_dir)
{
    wchar_t base[MAX_PATH], dir[MAX_PATH], game[MAX_PATH];
    uint32_t title = recomp_config_title_id();
    uint8_t r;

    save_base(base, MAX_PATH);
    game[0] = 0;
    if (game_dir && *game_dir)
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, game, MAX_PATH);
    for (r = 0; r < 2; r++) {
        swprintf(dir, MAX_PATH, L"%s\\%s\\%08X", base, k_roots[r], title);
        if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES && game[0] &&
            !getenv("RECOMP_SAVE_DIR"))
            swprintf(dir, MAX_PATH, L"%s\\%s\\%08X", game, k_roots[r], title);
        blob_tree(b, r, dir, "");
    }
}

static void delete_tree(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    swprintf(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            swprintf(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_tree(path);
            } else {
                SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* A path from the host, to be written under a folder of the joiner's: a
 * name or names under it, nothing that climbs out or names a drive. */
static int safe_rel(const char *rel, size_t n)
{
    size_t i;

    if (!n || rel[0] == '\\')
        return 0;
    for (i = 0; i < n; i++) {
        if (rel[i] == ':' || rel[i] == '/' || (unsigned char)rel[i] < 0x20)
            return 0;
        if (rel[i] == '.' && (i == 0 || rel[i - 1] == '\\') && i + 1 < n && rel[i + 1] == '.')
            return 0;
    }
    return 1;
}

/* The host's saves into the joiner's session folder, which is then the save
 * folder for this run (RECOMP_SAVE_DIR, read by kernel_path.c next). */
static int blob_write(const uint8_t *p, size_t n)
{
    wchar_t base[MAX_PATH], session[MAX_PATH], roots[2][MAX_PATH], path[MAX_PATH];
    wchar_t wrel[MAX_PATH];
    char rel[1024], env[MAX_PATH * 3];
    uint32_t title = recomp_config_title_id();
    size_t at = 0;
    int r;

    save_base(base, MAX_PATH);
    swprintf(session, MAX_PATH, L"%s\\online", base);
    for (r = 0; r < 2; r++) {
        swprintf(roots[r], MAX_PATH, L"%s\\%s\\%08X", session, k_roots[r], title);
        delete_tree(roots[r]);
        SHCreateDirectoryExW(NULL, roots[r], NULL);
    }
    while (at < n) {
        uint8_t root;
        uint16_t len;
        uint32_t size;

        if (n - at < 3)
            return 0;
        root = p[at];
        memcpy(&len, p + at + 1, 2);
        at += 3;
        if (root > 1 || len >= sizeof rel || n - at < (size_t)len + 4u)
            return 0;
        memcpy(rel, p + at, len);
        rel[len] = 0;
        memcpy(&size, p + at + len, 4);
        at += (size_t)len + 4u;
        if (!safe_rel(rel, len))
            return 0;
        MultiByteToWideChar(CP_UTF8, 0, rel, -1, wrel, MAX_PATH);
        swprintf(path, MAX_PATH, L"%s\\%s", roots[root], wrel);
        if (size == 0xFFFFFFFFu) {
            SHCreateDirectoryExW(NULL, path, NULL);
        } else {
            HANDLE f;
            DWORD put = 0;

            if (n - at < size)
                return 0;
            f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f == INVALID_HANDLE_VALUE)
                return 0;
            if (size && (!WriteFile(f, p + at, size, &put, NULL) || put != size)) {
                CloseHandle(f);
                return 0;
            }
            CloseHandle(f);
            at += size;
        }
    }
    WideCharToMultiByte(CP_UTF8, 0, session, -1, env, sizeof env, NULL, NULL);
    _putenv_s("RECOMP_SAVE_DIR", env);
    fprintf(stderr, "[NET] the host's saves are in %s for this session\n", env);
    fflush(stderr);
    return 1;
}

/* What both machines must agree on and the host decides, and what a session
 * fixes whatever either machine's settings say: the lockstep clock (without
 * it no two machines agree), no pause when the window loses the focus (a
 * pause pressed for one is a pause for both), and this machine's keyboard,
 * mouse and first controller all playing this machine's player. Read by the
 * runtime from here on (recomp_config_bool looks at the environment first). */
/* Both games on this one PC -- trying co-op out, or testing it: they would
 * both read every controller and keyboard. Then the host plays on the
 * keyboard and mouse alone and player 2 on the first controller
 * (keyboard_alone on both, player 2's copy reading its second port). */
static int peer_on_this_pc(void)
{
    char ip[64], mine[256], *p, *ctx = NULL;
    const char *e = getenv("RECOMP_NET_ONE_PC");       /* 0 or 1: the tests' say */

    if (e && *e)
        return strcmp(e, "0") != 0;
    if (!inet_ntop(AF_INET, &g_net.peer.sin_addr, ip, sizeof ip))
        return 0;
    if (strncmp(ip, "127.", 4) == 0)
        return 1;
    recomp_net_local_addrs(mine, sizeof mine);
    for (p = strtok_s(mine, ", ", &ctx); p; p = strtok_s(NULL, ", ", &ctx))
        if (strcmp(p, ip) == 0)
            return 1;
    return 0;
}

static void pin_settings(const uint8_t *set)
{
    int one_pc = peer_on_this_pc();

#if defined(_M_X64)
    /* The game's x87 sines, cosines and powers are the C runtime's, which on
     * x64 takes an FMA3 path on a CPU that has it -- and that path may round
     * the last bit differently. Two machines, one with FMA3 and one without,
     * would then part. The same path on both: the one every x64 CPU has. */
    _set_FMA3_enable(0);
#endif
    _putenv_s("RECOMP_LOCKSTEP_CLOCK", "1");
    _putenv_s("RECOMP_PAUSE_UNFOCUSED", "0");
    _putenv_s("RECOMP_KEYBOARD_ALONE", one_pc ? "1" : "0");
    g_net.input_port = one_pc && !g_net.host ? 1u : 0u;
    if (one_pc) {
        fprintf(stderr, "[NET] both games are on this PC: the host plays on the keyboard and "
                "mouse, player 2 on the first controller\n");
        fflush(stderr);
    }
    _putenv_s("RECOMP_SKIP_INTRO", set[SET_SKIP_INTRO] ? "1" : "0");
    _putenv_s("RECOMP_WIDESCREEN", set[SET_WIDESCREEN] ? "1" : "0");
}

/* Frames of input delay for a round trip: the other side's controls must
 * be there by the frame they are for, half a round trip on, with room for a
 * late packet. 2 on a LAN, 3 to 5 across a country. */
static int delay_for(unsigned rtt_ms)
{
    int d = 1 + (int)((rtt_ms / 2.0 + 6.0) / (1000.0 / 60.0) + 0.999);

    return d < 2 ? 2 : d > 15 ? 15 : d;
}

/* This build: two builds of the runtime need not run the game the same way.
 * From the executable's own header: its link time and its size. */
static uint32_t build_id(void)
{
    const uint8_t *m = (const uint8_t *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)m;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(m + dos->e_lfanew);

    return (nt->FileHeader.TimeDateStamp ^ (nt->OptionalHeader.SizeOfImage * 0x9E3779B1u)) +
           NET_PROTOCOL;
}

/* This PC's public address, found once, on a thread of its own: asking a
 * STUN server can take a second or two (recomp_net_addr.h). */
static volatile LONG g_pub_state;       /* 0 not asked, 1 asking, 2 found, 3 none */
static char g_pub_addr[64];

static DWORD WINAPI public_addr_thread(LPVOID unused)
{
    char a[64];
    int ok = recomp_net_public_addr(a, sizeof a, 1500);

    (void)unused;
    if (ok)
        memcpy(g_pub_addr, a, sizeof a);
    fprintf(stderr, "[NET] this PC's internet address: %s\n", ok ? a : "not found");
    fflush(stderr);
    InterlockedExchange(&g_pub_state, ok ? 2 : 3);
    return 0;
}

int hle_net_public_addr(char *out, size_t n)
{
    if (InterlockedCompareExchange(&g_pub_state, 1, 0) == 0)
        CloseHandle(CreateThread(NULL, 0, public_addr_thread, NULL, 0, NULL));
    if (g_pub_state == 2) {
        snprintf(out, n, "%s", g_pub_addr);
        return 1;
    }
    if (out && n)
        out[0] = '\0';
    return g_pub_state == 3 ? -1 : 0;
}

/* The host's window: the addresses to give player 2, the internet's first,
 * and what the router made of the port. A router whose own public address
 * is a private one, or not the one the internet sees, sits behind the
 * provider's NAT (CGNAT): no port it forwards can be reached. */
static void host_waiting_text(char *text, size_t n, const char *lan)
{
    LONG u = g_upnp;
    char pub[64], first_lan[64];
    int found = hle_net_public_addr(pub, sizeof pub);
    int cgnat = u == UPNP_OPEN && g_public[0] &&
                (recomp_net_addr_is_private(g_public) || (found > 0 && strcmp(g_public, pub) != 0));
    char inet[96], local[96];
    const char *comma;

    snprintf(first_lan, sizeof first_lan, "%s", lan);
    if ((comma = strchr(first_lan, ',')) != NULL)
        first_lan[comma - first_lan] = '\0';
    if (found > 0)
        snprintf(inet, sizeof inet, "%s:%u", pub, g_net.port);
    else
        snprintf(inet, sizeof inet, "%s", found < 0 ? "(this PC's internet address was not found)"
                                                    : "(finding this PC's internet address...)");
    if (first_lan[0])
        snprintf(local, sizeof local, "%s:%u", first_lan, g_net.port);
    else
        snprintf(local, sizeof local, "this PC's address, port %u", g_net.port);
    /* What the Copy button copies: the address player 2 is most likely to
     * need. */
    snprintf(g_status_copy, sizeof g_status_copy, "%s", found > 0 ? inet : first_lan[0] ? local : "");
    if (g_invite.code[0] && g_invite.published) {
        /* An invite: the code is all player 2 needs, wherever they are. */
        snprintf(g_status_copy, sizeof g_status_copy, "%s", g_invite.code);
        snprintf(text, n, "Waiting for player 2...\n\n"
                 "Invite code:  %s\n"
                 "Player 2 chooses Join and types it in -- no ports to open.\n%s\n"
                 "Or by address: %s over the internet, %s on this network.",
                 g_invite.code,
                 g_invite.my_nat == RECOMP_NAT_STRICT
                     ? "This connection is strict: a friend on a strict one too will not get "
                       "through (a VPN will).\n" : "",
                 found > 0 ? inet : "(finding it)", local);
        return;
    }
    snprintf(text, n, "%sHosting co-op on UDP port %u. Waiting for player 2...\n\n"
             "Over the internet, player 2 joins  %s\n%s\n\n"
             "On the same network:  %s",
             g_invite.code[0] ? "The invite relay could not be reached: give player 2 an "
                                "address instead.\n\n" : "",
             g_net.port, inet,
             cgnat ? "But your internet provider shares one address among customers (CGNAT): "
                     "nobody can reach this PC from the internet. Use a VPN (ZeroTier, "
                     "Tailscale) instead."
             : u == UPNP_TRYING ? "Asking the router to open the port..."
             : u == UPNP_OPEN ? "The router has opened the port."
             : u == UPNP_TAKEN ? "The router forwards this port to another PC: choose another."
             : "Forward this UDP port on the router to this PC, or both join one VPN.",
             local);
}

static int setup_host(uint32_t build, uint32_t game, const char *game_dir)
{
    Blob saves;
    uint8_t set[SET_COUNT];
    uint32_t hash;
    unsigned chunks;
    char text[700], lan[128];
    LONG shown_upnp, shown_pub;
    ULONGLONG last_punch = 0;

    memset(&saves, 0, sizeof saves);
    blob_saves(&saves, game_dir);
    if (saves.over) {
        free(saves.p);
        return setup_fail("This game's saves could not be read, or are larger than a co-op "
                          "session sends (8 MB).");
    }
    hash = fnv(saves.p, saves.n);
    chunks = (unsigned)((saves.n + CHUNK_BYTES - 1u) / CHUNK_BYTES);
    set[SET_SKIP_INTRO] = (uint8_t)recomp_config_bool("RECOMP_SKIP_INTRO", "skip_intro", 0);
    set[SET_WIDESCREEN] = (uint8_t)recomp_config_bool("RECOMP_WIDESCREEN", "widescreen", 0);
    fprintf(stderr, "[NET] hosting on UDP port %u; saves %u bytes\n", g_net.port,
            (unsigned)saves.n);
    fflush(stderr);
    recomp_net_local_addrs(lan, sizeof lan);
    g_status_with_copy = 1;             /* the host's window: the code or an address to copy */
    if (g_invite.code[0]) {
        /* Listening before telling: a joiner's answer can come at once. */
        status("Setting up the invite...");
        status_pump();
        g_invite.inv = recomp_invite_open(g_invite.code);
        if (g_invite.inv) {
            gather_mine();
            recomp_invite_listen(g_invite.inv, invite_heard, NULL);
            if (!invite_publish(build))
                fprintf(stderr, "[NET] invite: %s could not be reached\n",
                        recomp_invite_service());
            else
                fprintf(stderr, "[NET] invite %s: waiting on %s\n", g_invite.code,
                        recomp_invite_service());
            fflush(stderr);
        }
    }
    upnp_open();
    hle_net_public_addr(NULL, 0);
    shown_upnp = g_upnp;
    shown_pub = g_pub_state;
    host_waiting_text(text, sizeof text, lan);
    status(text);
    for (;;) {
        NetSetup s;
        struct sockaddr_in from;

        if (!status_pump()) {
            free(saves.p);
            status_close();
            upnp_close();
            invite_close();
            return 0;
        }
        if (!g_net.have_peer && g_invite.npeer && GetTickCount64() - last_punch >= 150) {
            invite_punch(PKT_PUNCH, 0, 0, 0);
            last_punch = GetTickCount64();
        }
        if (!g_net.have_peer && (g_upnp != shown_upnp || g_pub_state != shown_pub)) {
            shown_upnp = g_upnp;
            shown_pub = g_pub_state;
            host_waiting_text(text, sizeof text, lan);
            status(text);
        }
        if (!setup_recv(&s, &from, 30))
            continue;
        if (s.type == PKT_JOIN) {
            if (g_net.have_peer && !from_peer(&from)) {
                send_setup(&from, PKT_REFUSE, REFUSE_FULL, build, game, NULL, 0);
            } else if (s.a != NET_PROTOCOL || s.b != build) {
                send_setup(&from, PKT_REFUSE, REFUSE_BUILD, build, game, NULL, 0);
                fprintf(stderr, "[NET] %s tried to join with another build\n",
                        inet_ntoa(from.sin_addr));
                fflush(stderr);
            } else if (s.c != game) {
                send_setup(&from, PKT_REFUSE, REFUSE_GAME, build, game, NULL, 0);
                fprintf(stderr, "[NET] %s tried to join with other game files\n",
                        inet_ntoa(from.sin_addr));
                fflush(stderr);
            } else {
                if (!g_net.have_peer) {
                    g_net.peer = from;
                    g_net.have_peer = 1;
                    fprintf(stderr, "[NET] player 2 is joining from %s:%u\n",
                            inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                    fflush(stderr);
                    snprintf(text, sizeof text, "Player 2 is joining from %s.\n\n"
                             "Sending the saves...", inet_ntoa(from.sin_addr));
                    status(text);
                }
                send_setup(&from, PKT_OFFER, (uint32_t)saves.n, hash, 0, set, SET_COUNT);
            }
        } else if (!from_peer(&from)) {
            continue;
        } else if (s.type == PKT_NEED) {
            unsigned i, sent = 0;
            for (i = 0; i < chunks && sent < BURST; i++) {
                size_t at = (size_t)i * CHUNK_BYTES, len;
                if (i / 8u < s.len && ((s.data[i / 8u] >> (i % 8u)) & 1u))
                    continue;                /* there already */
                len = saves.n - at < CHUNK_BYTES ? saves.n - at : CHUNK_BYTES;
                send_setup(&from, PKT_CHUNK, i, 0, 0, saves.p + at, (unsigned)len);
                sent++;
            }
        } else if (s.type == PKT_PING) {
            send_setup(&from, PKT_PONG, s.a, s.b, 0, NULL, 0);
        } else if (s.type == PKT_READY) {
            if (!getenv("RECOMP_NET_DELAY"))
                g_net.delay = delay_for(s.a);
            g_net.rtt = s.a;
            send_setup(&from, PKT_GO, (uint32_t)g_net.delay, 0, 0, NULL, 0);
            fprintf(stderr, "[NET] player 2 is ready: %u ms there and back, %d frame(s) of "
                    "input delay\n", s.a, g_net.delay);
            fflush(stderr);
            break;
        }
    }
    invite_close();
    free(saves.p);
    pin_settings(set);
    status_close();
    return 1;
}

static int setup_join(uint32_t build, uint32_t game, const char *where)
{
    static uint8_t have[CHUNK_BYTES];        /* a bit per chunk of the saves */
    uint8_t *saves = NULL, set[SET_COUNT];
    uint32_t bytes = 0, hash = 0, rtt[8];
    unsigned chunks = 0, got = 0, nrtt = 0, ping = 0;
    int offered = 0, ready = 0, quiet_said = 0;
    uint32_t start = now_ms(), last_join = 0, last_need = 0, last_ping = 0, last_ready = 0;
    uint32_t last_pub = 0;
    char text[320];

    memset(set, 0, sizeof set);
    memset(have, 0, sizeof have);
    snprintf(text, sizeof text, "Joining %s...", where);
    status(text);
    for (;;) {
        NetSetup s;
        struct sockaddr_in from;
        uint32_t now = now_ms();

        if (!status_pump()) {
            free(saves);
            status_close();
            return 0;
        }
        if (!offered && (!last_join || now - last_join >= 250u)) {
            if (g_net.have_peer)
                send_setup(&g_net.peer, PKT_JOIN, NET_PROTOCOL, build, game, NULL, 0);
            else
                invite_punch(PKT_JOIN, NET_PROTOCOL, build, game);  /* each of the host's */
            last_join = now ? now : 1u;
        }
        if (!g_net.have_peer && g_invite.inv && now - last_pub >= 10000u) {
            if (last_pub)
                invite_publish(build);     /* in case the host missed it */
            last_pub = now ? now : 1u;
        }
        if (!g_net.have_peer && g_invite.code[0] && now - start >= 30000u) {
            char why[700];
            int mine = g_invite.my_nat == RECOMP_NAT_STRICT;
            int theirs = g_invite.peer_nat == RECOMP_NAT_STRICT;

            snprintf(why, sizeof why, "The host's game was found, but nothing gets through "
                     "between the two.\n\n%s\n\nWhat works instead: both join one VPN "
                     "(ZeroTier, Tailscale, Radmin VPN) and use the host's address on it, or "
                     "play through Steam Remote Play Together or Parsec.",
                     mine && theirs ? "Both connections are strict (each gives the game a "
                                      "new port for every other computer it talks to), and "
                                      "two strict connections cannot open a direct link."
                     : mine ? "This connection is strict (it gives the game a new port for "
                              "every other computer it talks to), and the host's is not "
                              "open enough to meet it."
                     : theirs ? "The host's connection is strict (it gives the game a new "
                                "port for every other computer it talks to), and this one "
                                "is not open enough to meet it."
                     : "Neither connection looks strict: a firewall may be blocking the game "
                       "on one of the PCs. Allow it through Windows Firewall on both and "
                       "try again.");
            free(saves);
            invite_close();
            return setup_fail(why);
        }
        if (!offered && !quiet_said && now - start >= 4000u) {
            quiet_said = 1;
            snprintf(text, sizeof text, "Joining %s...\n\nNo answer yet. %s", where,
                     g_invite.code[0] ? "Opening the way through both routers..."
                                      : "Is the host's game started and waiting, and its port "
                                        "open to this machine?");
            status(text);
        }
        if (offered && got < chunks && now - last_need >= 40u) {
            send_setup(&g_net.peer, PKT_NEED, 0, 0, 0, have, (chunks + 7u) / 8u);
            last_need = now;
        }
        if (offered && nrtt < 5 && now - last_ping >= 50u) {
            send_setup(&g_net.peer, PKT_PING, ping++, now, 0, NULL, 0);
            last_ping = now;
        }
        if (offered && !ready && got == chunks && nrtt >= 3) {
            if (fnv(saves, bytes) != hash) {
                free(saves);
                return setup_fail("The host's saves arrived damaged. Try joining again.");
            }
            if (!blob_write(saves, bytes)) {
                free(saves);
                return setup_fail("The host's saves could not be written on this machine.");
            }
            ready = 1;
            status("Starting...");
        }
        if (ready && now - last_ready >= 200u) {
            unsigned i, j;
            for (i = 1; i < nrtt; i++)       /* the middle one: one slow ping is not the line */
                for (j = i; j > 0 && rtt[j - 1] > rtt[j]; j--) {
                    uint32_t t = rtt[j];
                    rtt[j] = rtt[j - 1];
                    rtt[j - 1] = t;
                }
            g_net.rtt = rtt[nrtt / 2];
            send_setup(&g_net.peer, PKT_READY, rtt[nrtt / 2], 0, 0, NULL, 0);
            last_ready = now;
        }
        if (!setup_recv(&s, &from, 10))
            continue;
        if (!g_net.have_peer && !invite_adopt(&from))
            continue;
        if (!from_peer(&from))
            continue;
        if (s.type == PKT_REFUSE) {
            free(saves);
            invite_close();
            return setup_fail(s.a == REFUSE_BUILD
                ? "The host is running a different build of the game. Both players need the "
                  "same release."
                : s.a == REFUSE_GAME
                ? "The host's game files differ from this machine's (another region or "
                  "version). Both players need the same game."
                : "The host already has a player 2.");
        } else if (s.type == PKT_OFFER && !offered) {
            if (s.a > MAX_SAVE)
                return setup_fail("The host's saves are larger than a co-op session sends.");
            bytes = s.a;
            hash = s.b;
            chunks = (bytes + CHUNK_BYTES - 1u) / CHUNK_BYTES;
            saves = (uint8_t *)malloc(bytes ? bytes : 1u);
            if (!saves)
                return setup_fail("Out of memory.");
            memcpy(set, s.data, s.len < SET_COUNT ? s.len : SET_COUNT);
            offered = 1;
            snprintf(text, sizeof text, "Joining %s...\n\nReceiving the host's saves...", where);
            status(text);
        } else if (s.type == PKT_CHUNK && offered && s.a < chunks &&
                   !((have[s.a / 8u] >> (s.a % 8u)) & 1u)) {
            size_t at = (size_t)s.a * CHUNK_BYTES;
            size_t len = bytes - at < CHUNK_BYTES ? bytes - at : CHUNK_BYTES;
            if (s.len == len) {
                memcpy(saves + at, s.data, len);
                have[s.a / 8u] |= (uint8_t)(1u << (s.a % 8u));
                got++;
            }
        } else if (s.type == PKT_PONG && nrtt < 8) {
            rtt[nrtt++] = now_ms() - s.b;
        } else if (s.type == PKT_GO && ready) {
            g_net.delay = (int)s.a;
            if (g_net.delay < 1) g_net.delay = 1;
            if (g_net.delay > 30) g_net.delay = 30;
            break;
        }
    }
    invite_close();
    free(saves);
    pin_settings(set);
    status_close();
    return 1;
}

/* ------------------------------------------- into a session, and out again */

/* The environment this game started with, before a session pinned anything
 * in it (pin_settings, blob_write): what a restart starts from, so leaving a
 * session leaves its settings and the joiner's save folder behind too. */
static wchar_t *g_env0;

static void env_snapshot(void)
{
    wchar_t *e = GetEnvironmentStringsW(), *p;
    size_t n;

    if (!e || g_env0)
        return;
    for (p = e; *p; p += wcslen(p) + 1)
        ;
    n = (size_t)(p - e) + 1;
    g_env0 = (wchar_t *)malloc(n * sizeof *e);
    if (g_env0)
        memcpy(g_env0, e, n * sizeof *e);
    FreeEnvironmentStringsW(e);
}

/* An environment entry's name sorts after `name` (the block is kept in
 * order, as CreateProcess asks). */
static int name_after(const wchar_t *entry, const wchar_t *name)
{
    const wchar_t *eq = wcschr(entry + 1, L'=');
    size_t len = eq ? (size_t)(eq - entry) : wcslen(entry);
    wchar_t buf[256];

    if (len >= 256)
        len = 255;
    wmemcpy(buf, entry, len);
    buf[len] = 0;
    return _wcsicmp(buf, name) > 0;
}

int hle_net_restart(const char *spec)
{
    wchar_t exe[MAX_PATH], dir[MAX_PATH], add[300], *env, *w, *p;
    size_t total = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int added = 0;

    env_snapshot();                     /* if no session began: as it is now */
    if (!g_env0 || !GetModuleFileNameW(NULL, exe, MAX_PATH) || !GetCurrentDirectoryW(MAX_PATH, dir))
        return 0;
    add[0] = 0;
    if (spec && *spec)
        swprintf(add, 300, L"RECOMP_NET=%hs", spec);
    for (p = g_env0; *p; p += wcslen(p) + 1)
        total += wcslen(p) + 1;
    env = (wchar_t *)malloc((total + wcslen(add) + 2) * sizeof *env);
    if (!env)
        return 0;
    w = env;
    for (p = g_env0; *p; p += wcslen(p) + 1) {
        if (_wcsnicmp(p, L"RECOMP_NET=", 11) == 0)
            continue;
        if (add[0] && !added && p[0] != L'=' && name_after(p, L"RECOMP_NET")) {
            wcscpy(w, add);
            w += wcslen(add) + 1;
            added = 1;
        }
        wcscpy(w, p);
        w += wcslen(p) + 1;
    }
    if (add[0] && !added) {
        wcscpy(w, add);
        w += wcslen(add) + 1;
    }
    *w = 0;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, NULL, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, env, dir,
                        &si, &pi)) {
        free(env);
        return 0;
    }
    free(env);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    fprintf(stderr, "[NET] restarting %s\n", spec && *spec ? spec : "on its own");
    fflush(stderr);
    xbox_HostExit(spec && *spec ? "restarting into co-op" : "restarting out of co-op");
    return 1;
}

int hle_net_local_input_port(void)
{
    return (int)g_net.input_port;
}

int hle_net_supported(void)
{
    return g_net.supported;
}

int hle_net_session_info(char *peer, size_t n, int *delay, unsigned *rtt_ms)
{
    if (!g_net.active || !g_net.have_peer)
        return 0;
    if (peer && n) {
        char ip[64];
        inet_ntop(AF_INET, &g_net.peer.sin_addr, ip, sizeof ip);
        snprintf(peer, n, "%s", ip);
    }
    if (delay)
        *delay = g_net.delay;
    if (rtt_ms)
        *rtt_ms = g_net.rtt;
    return 1;
}

/* Joining by an invite: the host's addresses from the relay -- the host
 * may not have started yet, so asked again every few seconds for a minute
 * and a half -- then this game's, for the host to open its router to. */
static int find_host(uint32_t build)
{
    ULONGLONG start = GetTickCount64(), last = 0;
    int reached = 0;
    char text[400];

    snprintf(text, sizeof text, "Joining with invite %s...\n\nLooking for the host's game...",
             g_invite.code);
    status(text);
    status_pump();
    g_invite.inv = recomp_invite_open(g_invite.code);
    if (!g_invite.inv)
        return setup_fail("Invite codes are turned off on this PC (RECOMP_NET_SIGNAL=0). Ask "
                          "the host for their address instead.");
    gather_mine();
    while (!g_invite.npeer) {
        if (!status_pump()) {
            invite_close();
            status_close();
            return 0;
        }
        if (!last || GetTickCount64() - last >= 2500) {
            int r = recomp_invite_poll(g_invite.inv, 30, invite_heard, NULL);
            reached |= r >= 0;
            last = GetTickCount64();
            if (!reached && GetTickCount64() - start > 12000) {
                snprintf(text, sizeof text, "%s, the invite relay, could not be reached. Check "
                         "this PC's internet connection, or ask the host for their address.",
                         recomp_invite_service());
                invite_close();
                return setup_fail(text);
            }
        }
        if (GetTickCount64() - start > 90000) {
            snprintf(text, sizeof text, "No game is waiting with invite %s. Has the host chosen "
                     "Host co-op, and is the code right?", g_invite.code);
            invite_close();
            return setup_fail(text);
        }
        Sleep(30);
    }
    if (!invite_publish(build))
        fprintf(stderr, "[NET] invite: could not tell the host where this game is; trying "
                "anyway\n");
    return 1;
}

int hle_net_session_begin(const void *xbe, size_t xbe_size, const char *game_dir)
{
    const char *spec = getenv("RECOMP_NET");
    uint32_t build, game;
    int ok;

    g_net.configured = 1;
    g_net.supported = 1;
    env_snapshot();
    if (!spec || !*spec)
        return 1;
    /* The settings file is named for the title, and this runs before the
     * memory layout, which would otherwise tell the settings which title it
     * is: from the XBE's certificate, as it does. */
    {
        const uint8_t *x = (const uint8_t *)xbe;
        uint32_t base_va, cert_va;

        if (xbe_size >= 0x11C) {
            memcpy(&base_va, x + 0x104, 4);
            memcpy(&cert_va, x + 0x118, 4);
            if (cert_va >= base_va && (uint64_t)(cert_va - base_va) + 0x0C <= xbe_size) {
                uint32_t title;
                memcpy(&title, x + (cert_va - base_va) + 0x08, 4);
                recomp_config_set_title(title);
            }
        }
    }
    build = build_id();
    game = fnv(xbe, xbe_size);
    if (!open_socket(spec)) {
        char text[300];
        snprintf(text, sizeof text, "Could not start co-op (RECOMP_NET=%s): the address was "
                 "not found, or the port is in use.", spec);
        return setup_fail(text);
    }
    if (g_net.host) {
        ok = setup_host(build, game, game_dir);
    } else if (g_invite.code[0]) {
        char where[160];
        snprintf(where, sizeof where, "the host (invite %s)", g_invite.code);
        ok = find_host(build) && setup_join(build, game, where);
    } else {
        char where[160];
        snprintf(where, sizeof where, "%s", spec + 5);
        ok = setup_join(build, game, where);
    }
    if (!ok)
        return 0;
    g_net.connected = 1;
    g_net.heard_at = GetTickCount64();
    start_lockstep();
    return 1;
}

int hle_net_active(void)
{
    if (!g_net.configured)
        configure();
    return g_net.active;
}

void hle_net_hash_range(uint32_t va, uint32_t size)
{
    g_net.hash_va = va;
    g_net.hash_size = size & ~7u;
}

unsigned hle_net_ports(void)
{
    return hle_net_active() ? 3u : 0u;
}

int hle_net_local_port(void)
{
    return hle_net_active() ? (int)g_net.me : -1;
}

void hle_net_add_local_aim(float dx, float dy)
{
    if (!hle_net_active())
        return;
    EnterCriticalSection(&g_net.lock);
    g_net.pending_aim[0] += dx;
    g_net.pending_aim[1] += dy;
    LeaveCriticalSection(&g_net.lock);
}

int hle_net_port_aim(unsigned port, float *dx, float *dy)
{
    if (!hle_net_active() || port > 1)
        return 0;
    *dx = g_net.now_aim[port][0];
    *dy = g_net.now_aim[port][1];
    g_net.now_aim[port][0] = g_net.now_aim[port][1] = 0.0f;   /* taken once */
    return 1;
}

int hle_net_port_input(unsigned port, RecompInputGamepad *out)
{
    if (!hle_net_active() || port > 1)
        return 0;
    *out = g_net.now[port];
    return 1;
}

const char *hle_net_notice(void)
{
    if (!g_net.active)
        return NULL;
    if (g_net.desynced)
        return "Co-op: the two games have drifted apart. Leave and start co-op again to play on.";
    if (g_net.slow_at && GetTickCount64() - g_net.slow_at < 3000)
        return "Co-op: waiting for the other player's game...";
    return NULL;
}

/* Until the other side is there: the first frame waits for it, so both
 * start their lockstep from the same frame. */
static void wait_connected(void)
{
    ULONGLONG since = GetTickCount64(), said = 0;

    while (!g_net.connected) {
        if (!g_net.host)
            send_packet(PKT_HELLO, 0);
        WaitForSingleObject(g_net.arrived, 100);
        if (GetTickCount64() - since >= said + 3000) {
            said += 3000;
            fprintf(stderr, "[NET] waiting for %s...\n", g_net.host ? "player 2 to join" : "the host");
            fflush(stderr);
        }
    }
}

/* While a frame waits on the other machine: the game's window still
 * answers (its messages are otherwise seen only as a frame ends), and after
 * a moment a small window over it says what is happening -- the other game
 * is busy (loading: it still sends PKT_ALIVE), or has gone silent -- with a
 * way to quit. The game draws nothing while it waits, so this cannot be on
 * its screen. */
static void wait_pump(ULONGLONG waiting_since)
{
    static ULONGLONG said_at;
    ULONGLONG now = GetTickCount64(), silent = now - g_net.heard_at;
    MSG msg;

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            ExitProcess(0);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (now - waiting_since < 1500)
        return;
    if (!g_status || now - said_at >= 1000) {
        char text[300];

        if (g_net.peer_left)
            snprintf(text, sizeof text, "The other player has left the game.");
        else if (silent < 3000)
            snprintf(text, sizeof text, "Waiting for the other player's game (%u s)...\n\n"
                     "It is still there: loading, most likely.",
                     (unsigned)((now - waiting_since) / 1000));
        else
            snprintf(text, sizeof text, "The other player's game has not answered for %u "
                     "seconds.\n\nIt may have closed, or the connection dropped. The game "
                     "carries on if it comes back.", (unsigned)(silent / 1000));
        status_show(text, L"Quit", 1);
        said_at = now;
    }
    if (!status_pump()) {
        fprintf(stderr, "[NET] quit while waiting for the other player\n");
        fflush(stderr);
        xbox_HostExit("quit while waiting for the other player");
    }
}

static void (*g_local_filter)(RecompInputGamepad *);

void hle_net_set_local_filter(void (*filter)(RecompInputGamepad *controls))
{
    g_local_filter = filter;
}

void hle_net_frame_end(unsigned long frame)
{
    RecompInputGamepad local;
    uint32_t target, next = (uint32_t)frame + 1u;
    ULONGLONG since, resent, began;

    if (!hle_net_active())
        return;
    if (!g_net.connected)
        wait_connected();

    /* This machine's controls, for `delay` frames on. */
    target = next + (uint32_t)g_net.delay;
    recomp_input_host_sample_local(g_net.input_port, &local);
    if (g_local_filter)
        g_local_filter(&local);
    EnterCriticalSection(&g_net.lock);
    to_net(&local, &g_net.mine[target % RING]);
    g_net.mine[target % RING].aim_x = g_net.pending_aim[0];
    g_net.mine[target % RING].aim_y = g_net.pending_aim[1];
    g_net.pending_aim[0] = g_net.pending_aim[1] = 0.0f;
    g_net.mine_frame[target % RING] = target + 1u;
    if (frame % HASH_EVERY == 0) {
        unsigned k = (unsigned)((frame / HASH_EVERY) % (RING / HASH_EVERY + 1));
        g_net.my_hash[k] = state_hash();
        g_net.my_hash_frame[k] = (uint32_t)frame;
        check_hashes();
    }
    LeaveCriticalSection(&g_net.lock);
    send_packet(PKT_INPUTS, target);

    /* The next frame's controls from both. The first frames, before the
     * delay has filled, are nobody's: an untouched pad on both machines. */
    if (next <= (uint32_t)g_net.delay + 1u) {
        memset(g_net.now, 0, sizeof g_net.now);
        memset(g_net.now_aim, 0, sizeof g_net.now_aim);
        return;
    }
    began = since = resent = GetTickCount64();
    for (;;) {
        int mine, theirs;
        EnterCriticalSection(&g_net.lock);
        mine = g_net.mine_frame[next % RING] == next + 1u;
        theirs = g_net.theirs_frame[next % RING] == next + 1u;
        if (mine && theirs) {
            from_net(&g_net.mine[next % RING], &g_net.now[g_net.me]);
            from_net(&g_net.theirs[next % RING], &g_net.now[g_net.me ^ 1u]);
            g_net.now_aim[g_net.me][0] = g_net.mine[next % RING].aim_x;
            g_net.now_aim[g_net.me][1] = g_net.mine[next % RING].aim_y;
            g_net.now_aim[g_net.me ^ 1u][0] = g_net.theirs[next % RING].aim_x;
            g_net.now_aim[g_net.me ^ 1u][1] = g_net.theirs[next % RING].aim_y;
        }
        LeaveCriticalSection(&g_net.lock);
        if (mine && theirs) {
            if (g_status)
                status_close();         /* the wait's window, gone with the wait */
            break;
        }
        WaitForSingleObject(g_net.arrived, 2);
        if (GetTickCount64() - resent >= 30) {
            send_packet(PKT_INPUTS, target);
            resent = GetTickCount64();
        }
        if (GetTickCount64() - began >= 250)
            g_net.slow_at = GetTickCount64();
        if (GetTickCount64() - since >= 2000) {
            fprintf(stderr, "[NET] frame %u: waiting for the other player's controls\n", next);
            fflush(stderr);
            since = GetTickCount64();
        }
        wait_pump(began);
    }
}

#else  /* not Windows: no netplay yet */

int  hle_net_session_begin(const void *xbe, size_t xbe_size, const char *game_dir)
{
    (void)xbe; (void)xbe_size; (void)game_dir;
    return 1;
}
int  hle_net_active(void) { return 0; }
void hle_net_frame_end(unsigned long frame) { (void)frame; }
int  hle_net_port_input(unsigned port, RecompInputGamepad *out) { (void)port; (void)out; return 0; }
unsigned hle_net_ports(void) { return 0; }
int  hle_net_local_port(void) { return -1; }
void hle_net_add_local_aim(float dx, float dy) { (void)dx; (void)dy; }
int  hle_net_port_aim(unsigned port, float *dx, float *dy) { (void)port; (void)dx; (void)dy; return 0; }
void hle_net_hash_range(uint32_t va, uint32_t size) { (void)va; (void)size; }
const char *hle_net_notice(void) { return NULL; }
int  hle_net_supported(void) { return 0; }
int  hle_net_local_input_port(void) { return 0; }
void hle_net_set_local_filter(void (*filter)(RecompInputGamepad *controls)) { (void)filter; }
int  hle_net_restart(const char *spec) { (void)spec; return 0; }
int  hle_net_public_addr(char *out, size_t n) { if (out && n) out[0] = '\0'; return -1; }
int  hle_net_session_info(char *peer, size_t n, int *delay, unsigned *rtt_ms)
{
    (void)peer; (void)n; (void)delay; (void)rtt_ms;
    return 0;
}
void hle_net_copy_text(const char *text) { (void)text; }

#endif
