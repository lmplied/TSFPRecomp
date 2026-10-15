/*
 * hle_xmv_play.h -- actually playing a movie behind the XMV replacement.
 *
 * hle_xmv.c answers the library's calls; this decodes the file those calls
 * name (src/video: xmv_demux, xmv_decode), puts each picture on the host's
 * movie layer (d3d8_movie) and the sound on a voice of its own
 * (RECOMP_AUDIO_SLOT_MOVIE). docs/technical/video-playback.md has the design.
 *
 * Every function is safe to call when playback is unavailable: open returns
 * 0 and hle_xmv.c keeps its old behaviour of reporting the movie over.
 */
#ifndef XBOXRECOMP_HLE_XMV_PLAY_H
#define XBOXRECOMP_HLE_XMV_PLAY_H

#include <stdint.h>

/* Opens the movie `source_va` names (a guest string, the path the title gave
 * XMVPlaybackCreate). Returns a handle > 0 with the picture size, or 0. */
int  xmv_play_open(uint32_t source_va, uint32_t *width, uint32_t *height);
void xmv_play_start(int handle);

/* One poll: 0 no new picture, 1 a new picture (on the movie layer, and in
 * `surface_va` when that is a surface this can write), 2 the movie is over.
 * With a new picture, *pts_ms is its time in the movie. */
uint32_t xmv_play_update(int handle, uint32_t surface_va, uint32_t *pts_ms);

/* Milliseconds into the movie, on the clock its pictures' times are on: what
 * XMVPlaybackGetCurrentTime answers for a movie played here. */
uint32_t xmv_play_time(int handle);

void xmv_play_close(int handle);

/* The title's logo videos, the ones it plays before its menu: file names
 * separated by ';', case ignored, '*' matching anything ("eag_*.xmv;frd.xmv").
 * With skip_intro on (RECOMP_SKIP_INTRO) each is reported over the moment it
 * is created, as a movie that cannot be played always was. Set by the title
 * project before the game starts. */
void xmv_set_intro_videos(const char *names);

/* 1 when `path` is one of those and skip_intro is on. */
int  xmv_is_skipped_intro(const char *path);

#endif
