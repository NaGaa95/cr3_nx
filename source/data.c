/* data.c -- Chaos Rings 3 data layer (loose-file AAsset + path resolution)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <switch.h>

#include "data.h"
#include "util.h"

static char g_dir[512] = ".";

void data_init(const char *argv0) {
  if (argv0 && argv0[0]) {
    snprintf(g_dir, sizeof(g_dir), "%s", argv0);
    // strip the file name, keeping the directory (handles both '/' and the
    // "sdmc:/..." device-prefixed paths libnx hands us)
    char *slash = strrchr(g_dir, '/');
    if (slash) {
      *slash = '\0';
    } else {
      strcpy(g_dir, ".");
    }
  }
  if (!g_dir[0])
    strcpy(g_dir, ".");
}

const char *data_dir(void) { return g_dir; }

char *data_path(const char *name, char *out, size_t outsz) {
  if (!name) name = "";
  // already absolute / device-qualified? use as-is.
  if (strchr(name, ':') || name[0] == '/')
    snprintf(out, outsz, "%s", name);
  else
    snprintf(out, outsz, "%s/%s", g_dir, name);
  return out;
}

int data_exists(const char *name) {
  char path[768];
  struct stat st;
  return stat(data_path(name, path, sizeof(path)), &st) == 0;
}

// --- AAsset over loose files ------------------------------------------------
//
// The engine reads assets through AAsset_getBuffer (it imports neither
// AAsset_read nor AAsset_seek), including the big .mvgl archives, so we slurp
// the whole file into memory on open. To survive that we:
//   - basename-fallback: if "<built path>" is missing, retry just the file name
//     in the game folder (our data is flat, the engine builds nested paths);
//   - cache large archives by name so a re-open doesn't reload (e.g. the ~1 GB
//     main.10007.android.mvgl is read once).

typedef struct {
  char     path[768]; // resolved on-disk path
  uint8_t *mem;       // loaded lazily on first getBuffer (NULL until then)
  size_t   size;      // known from stat() at open time
  size_t   pos;
  int      cached;    // 1 => mem owned by the cache, do not free on close
} Asset;

#define ACACHE_N 8
#define ACACHE_MIN (16u * 1024 * 1024) // only cache archives this big
static struct { char key[96]; uint8_t *mem; size_t size; } g_acache[ACACHE_N];
static Mutex g_acache_lock;

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

void *AAssetManager_fromJava(void *env, void *assetManager) {
  (void)env; (void)assetManager;
  return (void *)1; // any non-NULL token; we ignore the manager
}

// open is cheap: resolve the path and remember the size. The (potentially huge)
// archive body isn't read until the engine actually calls AAsset_getBuffer, so
// mounting all five archives at boot costs only a few stat()s.
void *AAssetManager_open(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  if (!filename)
    return NULL;

  char path[768];
  data_path(filename, path, sizeof(path));

  struct stat st;
  if (stat(path, &st) != 0) {
    // retry with just the basename in the game folder (flat layout)
    const char *b = base_name(filename);
    char p2[768];
    data_path(b, p2, sizeof(p2));
    if (strcmp(p2, path) != 0 && stat(p2, &st) == 0) {
      memcpy(path, p2, sizeof(path));
    } else {
      debugPrintf("AAsset: open(%s) MISSING\n", path);
      return NULL;
    }
  }

  Asset *a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  memcpy(a->path, path, sizeof(a->path));
  a->size = (size_t)st.st_size;
  a->mem = NULL;
  a->pos = 0;
  a->cached = 0;
  return a;
}

// load the body on demand, reusing a cached copy of big archives across re-opens
static int asset_load(Asset *a) {
  if (!a) return 0;
  if (a->mem) return 1;
  const char *key = base_name(a->path);

  mutexLock(&g_acache_lock);
  for (int i = 0; i < ACACHE_N; i++)
    if (g_acache[i].mem && !strcmp(g_acache[i].key, key)) { a->mem = g_acache[i].mem; a->cached = 1; break; }
  mutexUnlock(&g_acache_lock);
  if (a->mem) return 1;

  FILE *f = fopen(a->path, "rb");
  if (!f) { debugPrintf("AAsset: fopen(%s) FAIL\n", a->path); return 0; }
  uint8_t *mem = malloc(a->size ? a->size : 1);
  if (!mem) { fclose(f); debugPrintf("AAsset: OOM %lu for %s\n", (unsigned long)a->size, a->path); return 0; }
  const size_t got = a->size ? fread(mem, 1, a->size, f) : 0;
  fclose(f);
  if (got != a->size) { free(mem); debugPrintf("AAsset: short read %lu/%lu %s\n", (unsigned long)got, (unsigned long)a->size, a->path); return 0; }

  if (a->size >= ACACHE_MIN) {
    mutexLock(&g_acache_lock);
    for (int i = 0; i < ACACHE_N; i++)
      if (!g_acache[i].mem) {
        strncpy(g_acache[i].key, key, sizeof(g_acache[i].key) - 1);
        g_acache[i].mem = mem; g_acache[i].size = a->size; a->cached = 1;
        break;
      }
    mutexUnlock(&g_acache_lock);
  }
  a->mem = mem;
  return 1;
}

const void *AAsset_getBuffer(void *asset) {
  Asset *a = asset;
  if (!asset_load(a)) return NULL;
  return a->mem;
}

int64_t AAsset_getLength(void *asset) {
  Asset *a = asset;
  return a ? (int64_t)a->size : 0; // size is known without loading the body
}
int64_t AAsset_getLength64(void *asset) { return AAsset_getLength(asset); }

int AAsset_read(void *asset, void *buf, size_t count) {
  Asset *a = asset;
  if (!asset_load(a)) return -1;
  const size_t avail = a->size - a->pos;
  if (count > avail) count = avail;
  memcpy(buf, a->mem + a->pos, count);
  a->pos += count;
  return (int)count;
}

long AAsset_seek(void *asset, long off, int whence) {
  Asset *a = asset;
  if (!a) return -1;
  long base = (whence == SEEK_CUR) ? (long)a->pos : (whence == SEEK_END) ? (long)a->size : 0;
  long np = base + off;
  if (np < 0 || (size_t)np > a->size) return -1;
  a->pos = (size_t)np;
  return (long)a->pos;
}

void AAsset_close(void *asset) {
  Asset *a = asset;
  if (!a) return;
  if (a->mem && !a->cached) free(a->mem); // cached archives stay resident
  free(a);
}
