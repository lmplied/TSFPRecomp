/*
 * recomp_invite.c -- see recomp_invite.h.
 */
#include "recomp_invite.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_alphabet[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";   /* 31: no 0 O 1 I L */

int recomp_invite_parse(const char *text, char *code, size_t n)
{
    char raw[RECOMP_INVITE_CHARS + 1];
    size_t k = 0;
    const char *p = text;

    if (!text)
        return 0;
    while (*p == ' ')
        p++;
    if ((p[0] == 'F' || p[0] == 'f') && (p[1] == 'P' || p[1] == 'p'))
        p += 2;
    for (; *p; p++) {
        int c = toupper((unsigned char)*p);
        if (c == '-' || c == ' ')
            continue;
        if (!strchr(k_alphabet, c) || k >= RECOMP_INVITE_CHARS)
            return 0;                   /* a dot, a colon, a 0: an address */
        raw[k++] = (char)c;
    }
    if (k != RECOMP_INVITE_CHARS)
        return 0;
    raw[k] = 0;
    if (code && n)
        snprintf(code, n, "FP-%.4s-%.4s-%.4s", raw, raw + 4, raw + 8);
    return 1;
}

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

int recomp_invite_new(char *out, size_t n)
{
    unsigned char r[RECOMP_INVITE_CHARS];
    char raw[RECOMP_INVITE_CHARS + 1];
    int i;

    if (!NT_SUCCESS(BCryptGenRandom(NULL, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return 0;
    for (i = 0; i < RECOMP_INVITE_CHARS; i++)
        raw[i] = k_alphabet[r[i] % 31u];       /* 256 % 31 = 8: a bias of under 4% */
    raw[i] = 0;
    snprintf(out, n, "FP-%.4s-%.4s-%.4s", raw, raw + 4, raw + 8);
    return 1;
}

/* ------------------------------------------------------------ the relay */

static wchar_t g_host[128], g_base[128];        /* "ntfy.sh", and a path before the topic */
static INTERNET_PORT g_port;
static int g_secure, g_configured, g_off;
static char g_service[128];

static void configure(void)
{
    const char *e = getenv("RECOMP_NET_SIGNAL");
    wchar_t url[256], host[128], path[128];
    URL_COMPONENTS u;

    if (g_configured)
        return;
    g_configured = 1;
    if (e && strcmp(e, "0") == 0) {
        g_off = 1;
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, e && *e ? e : "https://ntfy.sh", -1, url, 256);
    memset(&u, 0, sizeof u);
    u.dwStructSize = sizeof u;
    u.lpszHostName = host;
    u.dwHostNameLength = 128;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 128;
    if (!WinHttpCrackUrl(url, 0, 0, &u)) {
        g_off = 1;
        return;
    }
    wcscpy_s(g_host, 128, host);
    wcscpy_s(g_base, 128, path);
    {
        size_t len = wcslen(g_base);
        if (len && g_base[len - 1] == L'/')
            g_base[len - 1] = 0;
    }
    g_port = u.nPort;
    g_secure = u.nScheme == INTERNET_SCHEME_HTTPS;
    WideCharToMultiByte(CP_UTF8, 0, g_host, -1, g_service, sizeof g_service, NULL, NULL);
}

const char *recomp_invite_service(void)
{
    configure();
    return g_service[0] ? g_service : "the invite relay";
}

struct RecompInvite {
    char      topic[64];
    BCRYPT_ALG_HANDLE aes;
    BCRYPT_KEY_HANDLE key;
    HINTERNET session;
    HANDLE    listener;
    HINTERNET listen_req, listen_conn;
    volatile LONG stop;
    void    (*got)(void *, const char *);
    void     *ctx;
};

static int sha256(const char *text, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg;
    NTSTATUS st;

    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
        return 0;
    st = BCryptHash(alg, NULL, 0, (PUCHAR)text, (ULONG)strlen(text), out, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    return NT_SUCCESS(st);
}

RecompInvite *recomp_invite_open(const char *code)
{
    RecompInvite *inv;
    char norm[32], seed[64];
    unsigned char h[32];
    int i;

    configure();
    if (g_off || !recomp_invite_parse(code, norm, sizeof norm))
        return NULL;
    inv = (RecompInvite *)calloc(1, sizeof *inv);
    if (!inv)
        return NULL;
    snprintf(seed, sizeof seed, "tsfp-coop-topic|%s", norm);
    if (!sha256(seed, h)) {
        free(inv);
        return NULL;
    }
    memcpy(inv->topic, "fpcoop", 6);
    for (i = 0; i < 12; i++)
        snprintf(inv->topic + 6 + i * 2, 3, "%02x", h[i]);
    snprintf(seed, sizeof seed, "tsfp-coop-key|%s", norm);
    if (!sha256(seed, h) ||
        !NT_SUCCESS(BCryptOpenAlgorithmProvider(&inv->aes, BCRYPT_AES_ALGORITHM, NULL, 0)) ||
        !NT_SUCCESS(BCryptSetProperty(inv->aes, BCRYPT_CHAINING_MODE,
                                      (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                      (ULONG)sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) ||
        !NT_SUCCESS(BCryptGenerateSymmetricKey(inv->aes, &inv->key, NULL, 0, h, 32, 0))) {
        recomp_invite_close(inv);
        return NULL;
    }
    inv->session = WinHttpOpen(L"TimeSplitters-FP-recomp/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!inv->session) {
        recomp_invite_close(inv);
        return NULL;
    }
    WinHttpSetTimeouts(inv->session, 5000, 5000, 5000, 10000);
    return inv;
}

void recomp_invite_close(RecompInvite *inv)
{
    if (!inv)
        return;
    InterlockedExchange(&inv->stop, 1);
    {
        HINTERNET r = (HINTERNET)InterlockedExchangePointer((PVOID *)&inv->listen_req, NULL);
        if (r)
            WinHttpCloseHandle(r);              /* ends the listener's read */
    }
    if (inv->listener) {
        WaitForSingleObject(inv->listener, 3000);
        CloseHandle(inv->listener);
    }
    if (inv->listen_conn)
        WinHttpCloseHandle(inv->listen_conn);
    if (inv->session)
        WinHttpCloseHandle(inv->session);
    if (inv->key)
        BCryptDestroyKey(inv->key);
    if (inv->aes)
        BCryptCloseAlgorithmProvider(inv->aes, 0);
    free(inv);
}

/* ---- sealing: nonce (12) | tag (16) | ciphertext, then base64 ---- */

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const unsigned char *in, size_t n, char *out)
{
    size_t i, o = 0;

    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i + 1] << 8 | in[i + 2];
        out[o++] = k_b64[v >> 18]; out[o++] = k_b64[(v >> 12) & 63];
        out[o++] = k_b64[(v >> 6) & 63]; out[o++] = k_b64[v & 63];
    }
    if (i < n) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0u);
        out[o++] = k_b64[v >> 18]; out[o++] = k_b64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? k_b64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

static size_t b64_decode(const char *in, unsigned char *out, size_t cap)
{
    unsigned v = 0;
    int bits = 0;
    size_t o = 0;

    for (; *in && *in != '"'; in++) {
        const char *at;
        if (*in == '\\' || *in == '=')
            continue;                   /* "\/" from a JSON writer; the padding */
        at = strchr(k_b64, *in);
        if (!at)
            return 0;
        v = v << 6 | (unsigned)(at - k_b64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap)
                return 0;
            out[o++] = (unsigned char)(v >> bits);
        }
    }
    return o;
}

static int seal(RecompInvite *inv, const char *text, char *out, size_t cap)
{
    unsigned char buf[12 + 16 + 1024];
    size_t n = strlen(text);
    ULONG done = 0;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;

    if (n > 1024 || (12 + 16 + n) * 4 / 3 + 8 > cap ||
        !NT_SUCCESS(BCryptGenRandom(NULL, buf, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return 0;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = buf;
    info.cbNonce = 12;
    info.pbTag = buf + 12;
    info.cbTag = 16;
    if (!NT_SUCCESS(BCryptEncrypt(inv->key, (PUCHAR)text, (ULONG)n, &info, NULL, 0, buf + 28,
                                  (ULONG)n, &done, 0)))
        return 0;
    b64_encode(buf, 28 + n, out);
    return 1;
}

static int unseal(RecompInvite *inv, const char *b64, char *out, size_t cap)
{
    unsigned char buf[12 + 16 + 1024];
    size_t n = b64_decode(b64, buf, sizeof buf);
    ULONG done = 0;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;

    if (n < 28 || n - 28 >= cap)
        return 0;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = buf;
    info.cbNonce = 12;
    info.pbTag = buf + 12;
    info.cbTag = 16;
    if (!NT_SUCCESS(BCryptDecrypt(inv->key, buf + 28, (ULONG)(n - 28), &info, NULL, 0,
                                  (PUCHAR)out, (ULONG)(n - 28), &done, 0)))
        return 0;                       /* not sealed with this code */
    out[done] = 0;
    return 1;
}

/* ---- HTTP ---- */

static HINTERNET request(RecompInvite *inv, HINTERNET *conn, const wchar_t *verb,
                         const wchar_t *path, const char *body)
{
    HINTERNET req;
    DWORD len = body ? (DWORD)strlen(body) : 0;

    *conn = WinHttpConnect(inv->session, g_host, g_port, 0);
    if (!*conn)
        return NULL;
    req = WinHttpOpenRequest(*conn, verb, path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, g_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req)
        return NULL;
    if (!WinHttpSendRequest(req, body ? L"Content-Type: text/plain\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
                            (DWORD)-1L, (LPVOID)body, len, len, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req);
        return NULL;
    }
    {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            WinHttpCloseHandle(req);
            return NULL;
        }
    }
    return req;
}

int recomp_invite_publish(RecompInvite *inv, const char *text)
{
    char body[1600];
    wchar_t path[256];
    HINTERNET conn = NULL, req;

    if (!inv || !seal(inv, text, body, sizeof body))
        return 0;
    swprintf(path, 256, L"%s/%hs", g_base, inv->topic);
    req = request(inv, &conn, L"POST", path, body);
    if (req)
        WinHttpCloseHandle(req);
    if (conn)
        WinHttpCloseHandle(conn);
    return req != NULL;
}

/* Each line of an ntfy JSON stream that is a message: its "message",
 * unsealed, to got. */
static int take_line(RecompInvite *inv, const char *line, void (*got)(void *, const char *),
                     void *ctx)
{
    const char *m;
    char text[1100];

    if (!strstr(line, "\"event\":\"message\"") || !(m = strstr(line, "\"message\":\"")))
        return 0;
    if (!unseal(inv, m + 11, text, sizeof text))
        return 0;
    got(ctx, text);
    return 1;
}

/* The response, line by line. Returns the messages handed over, or -1 when
 * reading failed before any. */
static int read_lines(RecompInvite *inv, HINTERNET req, void (*got)(void *, const char *),
                      void *ctx)
{
    enum { CAP = 16384 };
    char *buf = (char *)malloc(CAP);
    size_t have = 0;
    int count = 0;

    if (!buf)
        return -1;
    for (;;) {
        DWORD n = 0, ready = 0;
        char *nl;

        /* What has come, and no more: a read asked for more than that waits
         * for it, and a stream's lines come one at a time. */
        if (inv->stop || have >= CAP - 1 || !WinHttpQueryDataAvailable(req, &ready) ||
            (ready && !WinHttpReadData(req, buf + have,
                                       ready < CAP - 1 - have ? ready : (DWORD)(CAP - 1 - have),
                                       &n))) {
            free(buf);
            return count ? count : -1;
        }
        if (!n) {
            free(buf);
            return count;
        }
        have += n;
        buf[have] = 0;
        while ((nl = strchr(buf, '\n')) != NULL) {
            *nl = 0;
            count += take_line(inv, buf, got, ctx);
            have -= (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, have + 1);
        }
    }
}

int recomp_invite_poll(RecompInvite *inv, unsigned minutes,
                       void (*got)(void *ctx, const char *text), void *ctx)
{
    wchar_t path[256];
    HINTERNET conn = NULL, req;
    int n;

    if (!inv)
        return -1;
    swprintf(path, 256, L"%s/%hs/json?poll=1&since=%um", g_base, inv->topic, minutes);
    req = request(inv, &conn, L"GET", path, NULL);
    if (!req) {
        if (conn)
            WinHttpCloseHandle(conn);
        return -1;
    }
    n = read_lines(inv, req, got, ctx);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return n < 0 ? 0 : n;
}

static DWORD WINAPI listen_thread(LPVOID arg)
{
    RecompInvite *inv = (RecompInvite *)arg;
    wchar_t path[256];
    int said = 0;

    /* From a few minutes back, not only from now: a message posted while
     * the stream was still connecting is not lost (the caller's own, and a
     * game long gone, are told apart by what they say). */
    swprintf(path, 256, L"%s/%hs/json?since=5m", g_base, inv->topic);
    while (!inv->stop) {
        HINTERNET conn = NULL, req = request(inv, &conn, L"GET", path, NULL);

        if (!said++) {
            fprintf(stderr, "[INVITE] listening on %s: %s\n", g_service,
                    req ? "connected" : "could not connect; trying again");
            fflush(stderr);
        }
        if (req) {
            inv->listen_req = req;
            inv->listen_conn = conn;
            read_lines(inv, req, inv->got, inv->ctx);
            if (InterlockedCompareExchangePointer((PVOID *)&inv->listen_req, NULL, req) == req)
                WinHttpCloseHandle(req);
            inv->listen_conn = NULL;
        }
        if (conn)
            WinHttpCloseHandle(conn);
        if (!inv->stop)
            Sleep(2000);                /* the stream broke: again, after a moment */
    }
    return 0;
}

int recomp_invite_listen(RecompInvite *inv, void (*got)(void *ctx, const char *text), void *ctx)
{
    if (!inv)
        return 0;
    inv->got = got;
    inv->ctx = ctx;
    /* A stream stays open: its reads wait for the next message, and the
     * relay says it is alive every 45 seconds. */
    WinHttpSetTimeouts(inv->session, 5000, 5000, 5000, 120000);
    inv->listener = CreateThread(NULL, 0, listen_thread, inv, 0, NULL);
    return inv->listener != NULL;
}

/* ------------------------------------------------------------- STUN */

int recomp_net_stun_on(uintptr_t sock, char *ip, size_t n, unsigned *port, unsigned timeout_ms)
{
    static const char *const servers[][2] = {
        { "stun.l.google.com", "19302" },
        { "stun.cloudflare.com", "3478" },
    };
    SOCKET s = (SOCKET)sock;
    uint8_t req[20], resp[548];
    LARGE_INTEGER t;
    uint64_t x;
    size_t k;
    unsigned i;

    if (ip && n)
        ip[0] = 0;
    QueryPerformanceCounter(&t);
    x = (uint64_t)t.QuadPart ^ 0x9E3779B97F4A7C15ull;
    memset(req, 0, sizeof req);
    req[1] = 0x01;
    req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;
    for (i = 8; i < 20; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        req[i] = (uint8_t)(x >> 24);
    }
    for (k = 0; k < sizeof servers / sizeof servers[0]; k++) {
        struct addrinfo hints, *res = NULL;
        ULONGLONG until;

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(servers[k][0], servers[k][1], &hints, &res) != 0 || !res)
            continue;
        sendto(s, (const char *)req, sizeof req, 0, res->ai_addr, (int)res->ai_addrlen);
        until = GetTickCount64() + timeout_ms / 2;
        while (GetTickCount64() < until) {
            fd_set set;
            struct timeval tv = { 0, 100000 };
            int got;
            unsigned len, at;

            FD_ZERO(&set);
            FD_SET(s, &set);
            if (select(0, &set, NULL, NULL, &tv) <= 0)
                continue;
            got = recv(s, (char *)resp, sizeof resp, 0);
            if (got < 20 || resp[0] != 0x01 || resp[1] != 0x01 || memcmp(resp + 4, req + 4, 16))
                continue;                       /* not the answer */
            len = (unsigned)resp[2] << 8 | resp[3];
            if (20u + len > (unsigned)got)
                len = (unsigned)got - 20u;
            for (at = 20; at + 4 <= 20u + len;) {
                unsigned type = (unsigned)resp[at] << 8 | resp[at + 1];
                unsigned alen = (unsigned)resp[at + 2] << 8 | resp[at + 3];
                const uint8_t *v = resp + at + 4;
                if (at + 4 + alen > 20u + len)
                    break;
                if (alen >= 8 && v[1] == 0x01 && (type == 0x0020 || type == 0x0001)) {
                    uint32_t a = (uint32_t)v[4] << 24 | (uint32_t)v[5] << 16 |
                                 (uint32_t)v[6] << 8 | v[7];
                    unsigned p = (unsigned)v[2] << 8 | v[3];
                    if (type == 0x0020) {
                        a ^= 0x2112A442u;
                        p ^= 0x2112u;
                    }
                    snprintf(ip, n, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFFu,
                             (a >> 8) & 0xFFu, a & 0xFFu);
                    *port = p;
                    if (type == 0x0020) {
                        freeaddrinfo(res);
                        return 1;
                    }
                }
                at += 4 + ((alen + 3u) & ~3u);
            }
            freeaddrinfo(res);
            return ip[0] != 0;
        }
        freeaddrinfo(res);
    }
    return 0;
}

#else   /* not Windows */

int recomp_invite_new(char *out, size_t n) { (void)out; (void)n; return 0; }
RecompInvite *recomp_invite_open(const char *code) { (void)code; return NULL; }
void recomp_invite_close(RecompInvite *inv) { (void)inv; }
int recomp_invite_publish(RecompInvite *inv, const char *text) { (void)inv; (void)text; return 0; }
int recomp_invite_poll(RecompInvite *inv, unsigned minutes,
                       void (*got)(void *ctx, const char *text), void *ctx)
{
    (void)inv; (void)minutes; (void)got; (void)ctx;
    return -1;
}
int recomp_invite_listen(RecompInvite *inv, void (*got)(void *ctx, const char *text), void *ctx)
{
    (void)inv; (void)got; (void)ctx;
    return 0;
}
const char *recomp_invite_service(void) { return "the invite relay"; }
int recomp_net_stun_on(uintptr_t sock, char *ip, size_t n, unsigned *port, unsigned timeout_ms)
{
    (void)sock; (void)ip; (void)n; (void)port; (void)timeout_ms;
    return 0;
}

#endif
