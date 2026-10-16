/*
 * hle_texture_replace.h -- a title project's own images for some of the
 * title's textures.
 *
 * The shadow renderer makes a host texture for each guest texture it is
 * asked to bind, identified by what its texels are: the checksum the texture
 * cache keeps (hle_d3d8_texture.c, level0_checksum, over all its levels).
 * A replacer registered here is asked about each texture as it is made and
 * whenever its texels change, and can answer with an image of its own --
 * any power-of-two size, since a swizzled texture is addressed from 0 to 1 --
 * which is then what every draw samples. The title's layout is untouched:
 * only the picture inside the texture is different. Button prompts are the
 * first use (hle_prompts.h): the pad's A becomes the key bound to it.
 *
 * RECOMP_HLE_D3D8_TEX_DUMP=<folder> writes each small texture's bytes under
 * its checksum, which is how a title project finds the ones to replace.
 */
#ifndef XBOXRECOMP_HLE_TEXTURE_REPLACE_H
#define XBOXRECOMP_HLE_TEXTURE_REPLACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t        width, height;      /* each a power of two */
    const uint32_t *argb;               /* 0xAARRGGBB, rows top to bottom */
} XboxTextureImage;

/* -1: not a texture this replacer knows. 0: one it knows, drawn as the
 * title has it just now. 1: replaced, with the image in *out, which must
 * stay valid until the next call. On the thread that binds textures. */
typedef int (*xbox_TextureReplacer)(uint32_t checksum, uint32_t format, uint32_t width,
                                    uint32_t height, XboxTextureImage *out);

void xbox_D3D8SetTextureReplacer(xbox_TextureReplacer fn);

/* The replacer's answers have changed (prompts switched between a pad and
 * the keyboard, a key was rebound): every texture it knows is asked again at
 * its next bind. Any thread. */
void xbox_D3D8TextureReplacementsChanged(void);

#ifdef __cplusplus
}
#endif

#endif
