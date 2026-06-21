/* text2bitmap.c -- native Text2Bitmap (FreeType over the Switch shared fonts)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "text2bitmap.h"
#include "util.h"

// shared-font faces, tried in order for per-glyph CJK fallback
#define MAX_FACES 6
static FT_Library g_ft;
static FT_Face    g_faces[MAX_FACES];
static int        g_nfaces = 0;
static int        g_ready = 0;
// FT_Face objects are not reentrant and the engine may issue Text2Bitmap calls
// from more than one thread (asset/UI threads); serialise all g_faces access.
static Mutex      g_lock;

void text2bitmap_init(void) {
  if (g_ready)
    return;
  mutexInit(&g_lock);
  if (FT_Init_FreeType(&g_ft)) {
    debugPrintf("text2bitmap: FT_Init_FreeType failed\n");
    return;
  }
  static const PlSharedFontType types[] = {
    PlSharedFontType_Standard,
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
    PlSharedFontType_NintendoExt,
  };
  for (unsigned i = 0; i < sizeof(types) / sizeof(*types) && g_nfaces < MAX_FACES; i++) {
    PlFontData fd;
    if (R_FAILED(plGetSharedFontByType(&fd, types[i])))
      continue;
    if (FT_New_Memory_Face(g_ft, fd.address, fd.size, 0, &g_faces[g_nfaces]) == 0)
      g_nfaces++;
  }
  if (g_nfaces == 0) {
    debugPrintf("text2bitmap: no shared fonts loaded\n");
    return;
  }
  debugPrintf("text2bitmap: %d shared font face(s) loaded\n", g_nfaces);
  g_ready = 1;
}

// minimal UTF-8 decoder: returns the codepoint and advances *p
static uint32_t utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  uint32_t c = *s++;
  if (c >= 0xF0 && s[0] && s[1] && s[2]) {
    c = ((c & 0x07) << 18) | ((s[0] & 0x3F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    s += 3;
  } else if (c >= 0xE0 && s[0] && s[1]) {
    c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
    s += 2;
  } else if (c >= 0xC0 && s[0]) {
    c = ((c & 0x1F) << 6) | (s[0] & 0x3F);
    s += 1;
  }
  *p = (const char *)s;
  return c;
}

// pick the first shared face that has a glyph for cp, size it, and return it
static FT_Face face_for(uint32_t cp, int px) {
  for (int i = 0; i < g_nfaces; i++) {
    if (FT_Get_Char_Index(g_faces[i], cp)) {
      FT_Set_Pixel_Sizes(g_faces[i], 0, px);
      return g_faces[i];
    }
  }
  if (g_nfaces > 0) {
    FT_Set_Pixel_Sizes(g_faces[0], 0, px);
    return g_faces[0];
  }
  return NULL;
}

static int clamp_size(int px) {
  if (px <= 0) return 16;
  if (px > 256) return 256;
  return px;
}

// internal, unlocked (caller holds g_lock)
static int measure_width_l(const char *text, int pixel_size) {
  if (!g_ready || !text)
    return 0;
  const int px = clamp_size(pixel_size);
  int width = 0;
  const char *p = text;
  while (*p) {
    const uint32_t cp = utf8_next(&p);
    FT_Face f = face_for(cp, px);
    if (!f) continue;
    if (FT_Load_Char(f, cp, FT_LOAD_DEFAULT))
      continue;
    width += (int)(f->glyph->advance.x >> 6);
  }
  return width;
}

static int measure_height_l(int pixel_size) {
  const int px = clamp_size(pixel_size);
  if (g_ready && g_nfaces > 0) {
    FT_Set_Pixel_Sizes(g_faces[0], 0, px);
    const int h = (int)((g_faces[0]->size->metrics.ascender -
                         g_faces[0]->size->metrics.descender) >> 6);
    if (h > 0) return h;
  }
  return (px * 5) / 4;
}

int text2bitmap_measure_width(const char *text, int pixel_size) {
  if (!g_ready) return 0;
  mutexLock(&g_lock);
  const int w = measure_width_l(text, pixel_size);
  mutexUnlock(&g_lock);
  return w;
}

int text2bitmap_measure_height(const char *text, int pixel_size) {
  (void)text;
  if (!g_ready) return (clamp_size(pixel_size) * 5) / 4;
  mutexLock(&g_lock);
  const int h = measure_height_l(pixel_size);
  mutexUnlock(&g_lock);
  return h;
}

static FakeBitmap *render_l(const char *text, int pixel_size) {
  if (!g_ready)
    return NULL;
  const int px = clamp_size(pixel_size);

  // The engine displays exactly getTextHeight rows of the bitmap and crops any
  // excess at the TOP, so the cell height is fixed (= getTextHeight) and can't be
  // grown. The font's glyphs slightly overflow that cell at BOTH ends, so render
  // them a touch smaller and vertically centred -- that gives ascenders and
  // descenders (g j p q y) a little margin without changing the cell or position.
  const int cell_h = measure_height_l(px);
  const int bh = cell_h > 0 ? cell_h : 1;
  const int rpx = px - (px / 8 + 1);             // smaller glyph size -> margin both ends
  const int w = text ? measure_width_l(text, rpx) : 0;
  const int bw = w > 0 ? w : 1;

  FakeBitmap *bmp = calloc(1, sizeof(*bmp));
  if (!bmp)
    return NULL;
  bmp->tag = BITMAP_TAG;
  bmp->w = bw;
  bmp->h = bh;
  bmp->stride = bw;                            // 1 byte/pixel
  bmp->format = ANDROID_BITMAP_FORMAT_A_8;
  bmp->pixels = calloc((size_t)bw * bh, 1);
  if (!bmp->pixels) {
    free(bmp);
    return NULL;
  }
  if (!text || !text[0])
    return bmp;

  // vertically centre the (smaller) glyph content in the fixed cell
  FT_Set_Pixel_Sizes(g_faces[0], 0, rpx);
  const int content_h = (int)((g_faces[0]->size->metrics.ascender -
                               g_faces[0]->size->metrics.descender) >> 6);
  int top_pad = (bh - content_h) / 2;
  if (top_pad < 0) top_pad = 0;
  int baseline = top_pad + (int)(g_faces[0]->size->metrics.ascender >> 6);
  if (baseline <= 0 || baseline >= bh)
    baseline = (bh * 4) / 5;

  int pen_x = 0;
  const char *p = text;
  while (*p) {
    const uint32_t cp = utf8_next(&p);
    FT_Face f = face_for(cp, rpx);
    if (!f) continue;
    if (FT_Load_Char(f, cp, FT_LOAD_RENDER))
      continue;
    const FT_GlyphSlot g = f->glyph;
    const int gx = pen_x + g->bitmap_left;
    const int gy = baseline - g->bitmap_top;
    for (unsigned ry = 0; ry < g->bitmap.rows; ry++) {
      const int dy = gy + (int)ry;
      if (dy < 0 || dy >= bh) continue;
      const uint8_t *srow = g->bitmap.buffer + (size_t)ry * g->bitmap.pitch;
      for (unsigned rx = 0; rx < g->bitmap.width; rx++) {
        const int dx = gx + (int)rx;
        if (dx < 0 || dx >= bw) continue;
        const uint8_t a = srow[rx];
        if (!a) continue;
        // 8-bit coverage; the engine uploads this as an alpha texture and tints
        bmp->pixels[(size_t)dy * bw + dx] = a;
      }
    }
    pen_x += (int)(g->advance.x >> 6);
  }
  return bmp;
}

FakeBitmap *text2bitmap_render(const char *text, int pixel_size) {
  if (!g_ready)
    return NULL;
  mutexLock(&g_lock);
  FakeBitmap *bmp = render_l(text, pixel_size);
  mutexUnlock(&g_lock);
  return bmp;
}

void text2bitmap_free(FakeBitmap *bmp) {
  if (!bmp)
    return;
  free(bmp->pixels);
  free(bmp);
}

// ---------------------------------------------------------------------------
// AndroidBitmap NDK API (operates on FakeBitmap; the JNIEnv is ignored)
// ---------------------------------------------------------------------------

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  int32_t  format;
  uint32_t flags;
} AndroidBitmapInfo;

int AndroidBitmap_getInfo(void *env, void *jbitmap, void *info_) {
  (void)env;
  FakeBitmap *b = jbitmap;
  AndroidBitmapInfo *info = info_;
  if (!b || b->tag != BITMAP_TAG || !info)
    return -1; // ANDROID_BITMAP_RESULT_BAD_PARAMETER
  info->width = (uint32_t)b->w;
  info->height = (uint32_t)b->h;
  info->stride = (uint32_t)b->stride;
  info->format = b->format;
  info->flags = 0;
  return 0; // ANDROID_BITMAP_RESULT_SUCCESS
}

int AndroidBitmap_lockPixels(void *env, void *jbitmap, void **addrPtr) {
  (void)env;
  FakeBitmap *b = jbitmap;
  if (!b || b->tag != BITMAP_TAG || !addrPtr)
    return -1;
  *addrPtr = b->pixels;
  return 0;
}

int AndroidBitmap_unlockPixels(void *env, void *jbitmap) {
  (void)env; (void)jbitmap;
  return 0;
}
