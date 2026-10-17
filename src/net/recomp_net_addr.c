/*
 * recomp_net_addr.c -- see recomp_net_addr.h.
 */
#include "recomp_net_addr.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int recomp_net_addr_is_private(const char *ip)
{
    unsigned a, b;

    if (!ip || sscanf(ip, "%u.%u", &a, &b) != 2)
        return 0;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 100 && b >= 64 && b <= 127);
}

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

int recomp_net_local_addrs(char *out, size_t size)
{
    WSADATA wsa;
    char name[256];
    struct addrinfo hints, *res = NULL, *p;
    size_t n = 0;

    if (!out || !size)
        return 0;
    out[0] = '\0';
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (gethostname(name, sizeof name) == 0 && getaddrinfo(name, NULL, &hints, &res) == 0) {
        for (p = res; p; p = p->ai_next) {
            char ip[64];

            if (!inet_ntop(AF_INET, &((struct sockaddr_in *)p->ai_addr)->sin_addr, ip, sizeof ip) ||
                strncmp(ip, "127.", 4) == 0 || strncmp(ip, "169.254.", 8) == 0 ||
                n + strlen(ip) + 3 >= size)
                continue;
            n += (size_t)snprintf(out + n, size - n, "%s%s", n ? ", " : "", ip);
        }
        freeaddrinfo(res);
    }
    WSACleanup();
    return n > 0;
}

/* RFC 5389: a Binding Request, answered with the address and port the
 * request came from as the server saw it -- XOR-MAPPED-ADDRESS (0x0020),
 * the address XORed with the magic cookie, or the older MAPPED-ADDRESS
 * (0x0001) as it is. */
#define STUN_COOKIE 0x2112A442u

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int stun_ask(const char *host, const char *port, char *out, size_t n, unsigned timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    uint8_t req[20], resp[548];
    SOCKET s;
    LARGE_INTEGER t;
    int ok = 0, tries;
    unsigned i;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return 0;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return 0;
    }
    memset(req, 0, sizeof req);
    req[1] = 0x01;                                   /* Binding Request, no attributes */
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;
    /* The transaction id: anything a stray packet would not repeat. Not the C
     * runtime's rand(), whose one sequence the rest of the process shares. */
    QueryPerformanceCounter(&t);
    {
        uint64_t x = (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^
                     0x9E3779B97F4A7C15ull;
        for (i = 8; i < 20; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            req[i] = (uint8_t)(x >> 24);
        }
    }

    for (tries = 0; tries < 2 && !ok; tries++) {
        fd_set set;
        struct timeval tv;
        int got;

        sendto(s, (const char *)req, sizeof req, 0, res->ai_addr, (int)res->ai_addrlen);
        FD_ZERO(&set);
        FD_SET(s, &set);
        tv.tv_sec = (long)(timeout_ms / 2u / 1000u);
        tv.tv_usec = (long)(timeout_ms / 2u % 1000u) * 1000;
        if (select(0, &set, NULL, NULL, &tv) <= 0)
            continue;
        got = recv(s, (char *)resp, sizeof resp, 0);
        if (got < 20 || resp[0] != 0x01 || resp[1] != 0x01 || memcmp(resp + 4, req + 4, 16) != 0)
            continue;
        {
            unsigned len = (unsigned)resp[2] << 8 | resp[3], at = 20;
            uint32_t mapped = 0, xored = 0;

            if (20u + len > (unsigned)got)
                len = (unsigned)got - 20u;
            while (at + 4 <= 20u + len) {
                unsigned type = (unsigned)resp[at] << 8 | resp[at + 1];
                unsigned alen = (unsigned)resp[at + 2] << 8 | resp[at + 3];
                const uint8_t *v = resp + at + 4;

                if (at + 4 + alen > 20u + len)
                    break;
                if (alen >= 8 && v[1] == 0x01) {    /* IPv4 */
                    if (type == 0x0020)
                        xored = be32(v + 4) ^ STUN_COOKIE;
                    else if (type == 0x0001)
                        mapped = be32(v + 4);
                }
                at += 4 + ((alen + 3u) & ~3u);
            }
            if (!xored)
                xored = mapped;
            if (xored) {
                snprintf(out, n, "%u.%u.%u.%u", xored >> 24, (xored >> 16) & 0xFFu,
                         (xored >> 8) & 0xFFu, xored & 0xFFu);
                ok = 1;
            }
        }
    }
    closesocket(s);
    freeaddrinfo(res);
    return ok;
}

int recomp_net_public_addr(char *out, size_t n, unsigned timeout_ms)
{
    static const char *const servers[][2] = {
        { "stun.l.google.com", "19302" },
        { "stun.cloudflare.com", "3478" },
    };
    const char *e = getenv("RECOMP_NET_PUBLIC_IP");
    WSADATA wsa;
    size_t i;
    int ok = 0;

    if (!out || !n)
        return 0;
    out[0] = '\0';
    if ((e && *e && strcmp(e, "0") == 0) || WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;
    for (i = 0; i < sizeof servers / sizeof servers[0] && !ok; i++)
        ok = stun_ask(servers[i][0], servers[i][1], out, n, timeout_ms);
    WSACleanup();
    return ok;
}

#else   /* not Windows: no co-op yet */

int recomp_net_local_addrs(char *out, size_t n)
{
    if (out && n)
        out[0] = '\0';
    return 0;
}

int recomp_net_public_addr(char *out, size_t n, unsigned timeout_ms)
{
    (void)timeout_ms;
    if (out && n)
        out[0] = '\0';
    return 0;
}

#endif
