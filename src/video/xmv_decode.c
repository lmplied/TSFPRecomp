/*
 * xmv_decode.c -- see xmv_decode.h.
 *
 * FFmpeg's WMV2 decoder, loaded at run time. Windows' own WMV decoder lists
 * WMV2 and refuses to be set up for it (docs/technical/video-playback.md), so
 * it is not an option. Only FFmpeg's headers are used at build time; the
 * libraries are looked for when the first movie opens -- beside the
 * executable, then in RECOMP_FFMPEG_DIR, then in this checkout's
 * third_party/ffmpeg/bin -- and loaded by the major version the headers name,
 * so the structure layouts used here are the ones the library has. When they
 * are not there the movie is skipped, as it always was, and the title runs.
 */
#include "xmv_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(XMV_HAVE_FFMPEG)

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#if defined(_WIN32)
#include <windows.h>
typedef HMODULE lib_t;
static lib_t lib_open(const char *dir, const char *name)
{
    char path[MAX_PATH * 2];
    if (!dir)
        return LoadLibraryA(name);
    snprintf(path, sizeof path, "%s\\%s", dir, name);
    /* The altered search path lets avcodec find avutil and swresample in the
     * same directory rather than on PATH. */
    return LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
static void *lib_sym(lib_t l, const char *s) { return (void *)GetProcAddress(l, s); }
#define STR2(x) #x
#define STR(x) STR2(x)
#define AVCODEC_NAME    "avcodec-" STR(LIBAVCODEC_VERSION_MAJOR) ".dll"
#define AVUTIL_NAME     "avutil-" STR(LIBAVUTIL_VERSION_MAJOR) ".dll"
#define SWRESAMPLE_NAME "swresample-6.dll"
#else
#include <dlfcn.h>
typedef void *lib_t;
static lib_t lib_open(const char *dir, const char *name)
{
    char path[4096];
    if (!dir)
        return dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}
static void *lib_sym(lib_t l, const char *s) { return dlsym(l, s); }
#define STR2(x) #x
#define STR(x) STR2(x)
#define AVCODEC_NAME    "libavcodec.so." STR(LIBAVCODEC_VERSION_MAJOR)
#define AVUTIL_NAME     "libavutil.so." STR(LIBAVUTIL_VERSION_MAJOR)
#define SWRESAMPLE_NAME NULL
#endif

static struct {
    int tried, ok;
    const AVCodec *(*find_decoder_by_name)(const char *);
    AVCodecContext *(*alloc_context3)(const AVCodec *);
    int  (*open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
    void (*free_context)(AVCodecContext **);
    int  (*send_packet)(AVCodecContext *, const AVPacket *);
    int  (*receive_frame)(AVCodecContext *, AVFrame *);
    AVPacket *(*packet_alloc)(void);
    void (*packet_free)(AVPacket **);
    AVFrame *(*frame_alloc)(void);
    void (*frame_free)(AVFrame **);
    void *(*mallocz)(size_t);
} ff;

/* Load once; 1 if FFmpeg is usable. */
static int ff_load(void)
{
    const char *dirs[3];
    lib_t codec = NULL, util = NULL;
    int i;

    if (ff.tried)
        return ff.ok;
    ff.tried = 1;
    dirs[0] = NULL;                          /* beside the executable / system path */
    dirs[1] = getenv("RECOMP_FFMPEG_DIR");
#ifdef XMV_FFMPEG_BIN
    dirs[2] = XMV_FFMPEG_BIN;                /* this checkout, for development */
#else
    dirs[2] = NULL;
#endif
    for (i = 0; i < 3 && !codec; i++) {
        if (i > 0 && !dirs[i])
            continue;
        util = lib_open(dirs[i], AVUTIL_NAME);
        if (!util)
            continue;
        if (SWRESAMPLE_NAME)
            lib_open(dirs[i], SWRESAMPLE_NAME);
        codec = lib_open(dirs[i], AVCODEC_NAME);
    }
    if (!codec || !util) {
        fprintf(stderr, "[XMV] no FFmpeg (%s, %s) beside the executable, in "
                "RECOMP_FFMPEG_DIR or in third_party/ffmpeg/bin: movies are skipped\n",
                AVCODEC_NAME, AVUTIL_NAME);
        fflush(stderr);
        return 0;
    }
#define LOAD(field, lib, name) \
    if (!(*(void **)&ff.field = lib_sym(lib, name))) { \
        fprintf(stderr, "[XMV] FFmpeg is missing %s: movies are skipped\n", name); \
        return 0; }
    LOAD(find_decoder_by_name, codec, "avcodec_find_decoder_by_name");
    LOAD(alloc_context3, codec, "avcodec_alloc_context3");
    LOAD(open2, codec, "avcodec_open2");
    LOAD(free_context, codec, "avcodec_free_context");
    LOAD(send_packet, codec, "avcodec_send_packet");
    LOAD(receive_frame, codec, "avcodec_receive_frame");
    LOAD(packet_alloc, codec, "av_packet_alloc");
    LOAD(packet_free, codec, "av_packet_free");
    LOAD(frame_alloc, util, "av_frame_alloc");
    LOAD(frame_free, util, "av_frame_free");
    LOAD(mallocz, util, "av_mallocz");
#undef LOAD
    ff.ok = 1;
    fprintf(stderr, "[XMV] FFmpeg %s loaded for movie playback\n", AVCODEC_NAME);
    fflush(stderr);
    return 1;
}

struct xmv_video_decoder {
    AVCodecContext *ctx;
    AVPacket       *pkt;
    AVFrame        *frame;
    uint8_t        *buf;            /* input copy with FFmpeg's padding */
    size_t          buf_size;
    uint32_t        width, height;
};

xmv_video_decoder *xmv_decoder_create(uint32_t width, uint32_t height,
                                      const uint8_t extradata[4])
{
    const AVCodec *codec;
    xmv_video_decoder *d;

    if (!ff_load())
        return NULL;
    codec = ff.find_decoder_by_name("wmv2");
    if (!codec) {
        fprintf(stderr, "[XMV] this FFmpeg has no WMV2 decoder: movies are skipped\n");
        return NULL;
    }
    d = (xmv_video_decoder *)calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->width = width;
    d->height = height;
    d->ctx = ff.alloc_context3(codec);
    d->pkt = ff.packet_alloc();
    d->frame = ff.frame_alloc();
    if (!d->ctx || !d->pkt || !d->frame) {
        xmv_decoder_destroy(d);
        return NULL;
    }
    d->ctx->width = (int)width;
    d->ctx->height = (int)height;
    /* The decoder reads its codec data from here, padded, and frees it. */
    d->ctx->extradata = (uint8_t *)ff.mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->ctx->extradata) {
        xmv_decoder_destroy(d);
        return NULL;
    }
    memcpy(d->ctx->extradata, extradata, 4);
    d->ctx->extradata_size = 4;
    /* The same pictures from every CPU: co-op's two machines write them into
     * the title's memory (hle_xmv_play.c), and the two must stay alike. */
    d->ctx->flags |= AV_CODEC_FLAG_BITEXACT;
    d->ctx->thread_count = 1;
    if (ff.open2(d->ctx, codec, NULL) < 0) {
        fprintf(stderr, "[XMV] FFmpeg would not open WMV2 %ux%u\n", width, height);
        xmv_decoder_destroy(d);
        return NULL;
    }
    fprintf(stderr, "[XMV] video decoder: WMV2 %ux%u through FFmpeg\n", width, height);
    fflush(stderr);
    return d;
}

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* BT.601, studio range. */
static void yuv_to_bgra(int y, int u, int v, uint8_t *o)
{
    int c = y - 16, d = u - 128, e = v - 128;
    o[0] = clamp8((298 * c + 516 * d + 128) >> 8);
    o[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
    o[2] = clamp8((298 * c + 409 * e + 128) >> 8);
    o[3] = 0xFF;
}

/* WMV2 decodes to 4:2:0 planar, which is all this has to handle. */
static int to_bgra(const xmv_video_decoder *d, const AVFrame *f, uint8_t *bgra)
{
    uint32_t w = d->width, h = d->height, x, y;

    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P)
        return -1;
    if ((uint32_t)f->width < w) w = (uint32_t)f->width;
    if ((uint32_t)f->height < h) h = (uint32_t)f->height;
    for (y = 0; y < h; y++) {
        const uint8_t *yr = f->data[0] + (size_t)y * (size_t)f->linesize[0];
        const uint8_t *ur = f->data[1] + (size_t)(y / 2u) * (size_t)f->linesize[1];
        const uint8_t *vr = f->data[2] + (size_t)(y / 2u) * (size_t)f->linesize[2];
        uint8_t *out = bgra + (size_t)y * d->width * 4u;
        for (x = 0; x < w; x++)
            yuv_to_bgra(yr[x], ur[x / 2u], vr[x / 2u], out + x * 4u);
    }
    return 0;
}

int xmv_decoder_decode(xmv_video_decoder *dec, const uint8_t *data, uint32_t size,
                       uint32_t pts_ms, uint8_t *bgra)
{
    int r, got = 0;

    if (!dec)
        return -1;
    if (dec->buf_size < (size_t)size + AV_INPUT_BUFFER_PADDING_SIZE) {
        free(dec->buf);
        dec->buf_size = (size_t)size + AV_INPUT_BUFFER_PADDING_SIZE;
        dec->buf = (uint8_t *)malloc(dec->buf_size);
        if (!dec->buf) {
            dec->buf_size = 0;
            return -1;
        }
    }
    memcpy(dec->buf, data, size);
    memset(dec->buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    dec->pkt->data = dec->buf;
    dec->pkt->size = (int)size;
    dec->pkt->pts = pts_ms;
    r = ff.send_packet(dec->ctx, dec->pkt);
    dec->pkt->data = NULL;
    dec->pkt->size = 0;
    if (r < 0 && r != AVERROR(EAGAIN))
        return 0;                    /* a frame it cannot use: show the last one */
    /* WMV2 has no B-frames, so each packet gives at most one picture; drain
     * whatever there is and keep the last. */
    while (ff.receive_frame(dec->ctx, dec->frame) == 0) {
        if (to_bgra(dec, dec->frame, bgra) == 0)
            got = 1;
    }
    return got;
}

void xmv_decoder_destroy(xmv_video_decoder *dec)
{
    if (!dec)
        return;
    if (dec->ctx)
        ff.free_context(&dec->ctx);
    if (dec->pkt)
        ff.packet_free(&dec->pkt);
    if (dec->frame)
        ff.frame_free(&dec->frame);
    free(dec->buf);
    free(dec);
}

#else  /* !XMV_HAVE_FFMPEG: built without FFmpeg's headers */

xmv_video_decoder *xmv_decoder_create(uint32_t width, uint32_t height,
                                      const uint8_t extradata[4])
{
    static int said;
    (void)width; (void)height; (void)extradata;
    if (!said++)
        fprintf(stderr, "[XMV] built without FFmpeg headers (third_party/ffmpeg): "
                        "movies are skipped\n");
    return NULL;
}

int xmv_decoder_decode(xmv_video_decoder *dec, const uint8_t *data, uint32_t size,
                       uint32_t pts_ms, uint8_t *bgra)
{
    (void)dec; (void)data; (void)size; (void)pts_ms; (void)bgra;
    return -1;
}

void xmv_decoder_destroy(xmv_video_decoder *dec)
{
    (void)dec;
}

#endif
