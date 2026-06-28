/* android_native.c -- NativeActivity / native_app_glue host for libcrx.so
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <switch.h>

#include "android_native.h"
#include "config.h"
#include "util.h"

// --- NDK constants ----------------------------------------------------------
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define ALOOPER_POLL_ERROR    (-4)
#define ALOOPER_EVENT_INPUT   (1 << 0)

#define AINPUT_EVENT_TYPE_KEY    1
#define AINPUT_EVENT_TYPE_MOTION 2
#define AMOTION_ACTION_MOVE      2

// input event/queue types are defined up front so the fake-fd readability check
// (fd_readable_locked) can read queue->count.
struct AInputEvent {
  int32_t type;
  int32_t action;
  int     pcount;
  int32_t ids[8];
  float   xs[8];
  float   ys[8];
  int32_t keycode, flags, repeat;
  struct AInputEvent *next;
};

struct AInputQueue {
  ALooper *looper;
  int ident;
  int fd;                 // fake FD_INPUT
  AInputEvent *head, *tail;
  int count;
};

#define INPUT_QUEUE_SOFT_CAP 64

// ===========================================================================
// fake-fd layer: an in-process pipe + input-notifier backing for the glue's
// command pipe and the input queue. Fake fds live in a high numeric range so
// libc_shim can tell them apart from real newlib fds.
// ===========================================================================

#define FAKE_FD_BASE 0x40000000
#define MAX_FAKE_FDS 32
#define PIPE_CAP     4096

enum { FD_NONE = 0, FD_PIPE_R, FD_PIPE_W, FD_INPUT };

typedef struct {
  uint8_t buf[PIPE_CAP];
  size_t head, len;
  int refs;
} Pipe;

typedef struct {
  int kind;
  Pipe *pipe;            // FD_PIPE_R / FD_PIPE_W
  AInputQueue *queue;    // FD_INPUT
} FakeFd;

static FakeFd g_fds[MAX_FAKE_FDS];
static Mutex  g_lock;       // guards fds, pipes, queues and looper readiness
static CondVar g_cond;      // broadcast whenever any fd becomes readable
static int g_inited = 0;

static int alloc_slot(void) {
  for (int i = 0; i < MAX_FAKE_FDS; i++)
    if (g_fds[i].kind == FD_NONE)
      return i;
  return -1;
}

static int fd_readable_locked(int slot) {
  FakeFd *f = &g_fds[slot];
  if (f->kind == FD_PIPE_R) return f->pipe && f->pipe->len > 0;
  if (f->kind == FD_INPUT)  return f->queue && f->queue->count > 0; // count read below
  return 0;
}

int fakefd_is_fake(int fd) {
  return fd >= FAKE_FD_BASE && fd < FAKE_FD_BASE + MAX_FAKE_FDS;
}

int fakefd_pipe(int fds[2]) {
  mutexLock(&g_lock);
  int r = alloc_slot();
  if (r < 0) { mutexUnlock(&g_lock); return -1; }
  g_fds[r].kind = FD_PIPE_R;
  int w = alloc_slot();
  if (w < 0) { g_fds[r].kind = FD_NONE; mutexUnlock(&g_lock); return -1; }
  g_fds[w].kind = FD_PIPE_W;
  Pipe *p = calloc(1, sizeof(*p));
  if (!p) { g_fds[r].kind = g_fds[w].kind = FD_NONE; mutexUnlock(&g_lock); return -1; }
  p->refs = 2;
  g_fds[r].pipe = g_fds[w].pipe = p;
  mutexUnlock(&g_lock);
  fds[0] = FAKE_FD_BASE + r;
  fds[1] = FAKE_FD_BASE + w;
  return 0;
}

long fakefd_write(int fd, const void *buf, unsigned long n) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return -1;
  mutexLock(&g_lock);
  Pipe *p = g_fds[slot].pipe;
  if (g_fds[slot].kind != FD_PIPE_W || !p) { mutexUnlock(&g_lock); return -1; }
  size_t wrote = 0;
  const uint8_t *src = buf;
  while (wrote < n && p->len < PIPE_CAP) {
    const size_t tail = (p->head + p->len) % PIPE_CAP;
    p->buf[tail] = src[wrote++];
    p->len++;
  }
  condvarWakeAll(&g_cond);
  mutexUnlock(&g_lock);
  return (long)wrote;
}

long fakefd_read(int fd, void *buf, unsigned long n) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return -1;
  mutexLock(&g_lock);
  Pipe *p = g_fds[slot].pipe;
  if (g_fds[slot].kind != FD_PIPE_R || !p) { mutexUnlock(&g_lock); return -1; }
  size_t got = 0;
  uint8_t *dst = buf;
  while (got < n && p->len > 0) {
    dst[got++] = p->buf[p->head];
    p->head = (p->head + 1) % PIPE_CAP;
    p->len--;
  }
  mutexUnlock(&g_lock);
  return (long)got; // 0 if drained (the glue reads exactly what the looper signalled)
}

int fakefd_close(int fd) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return -1;
  mutexLock(&g_lock);
  FakeFd *f = &g_fds[slot];
  if (f->pipe && --f->pipe->refs <= 0)
    free(f->pipe);
  memset(f, 0, sizeof(*f));
  condvarWakeAll(&g_cond); // wake any poller blocked on this fd (treat as EOF)
  mutexUnlock(&g_lock);
  return 0;
}

// ===========================================================================
// ALooper -- a single global looper used by the android_main thread.
// ===========================================================================

#define MAX_POLL_ITEMS 8
typedef struct { int fd; int ident; int events; ALooper_callbackFunc cb; void *data; } PollItem;
struct ALooper { PollItem items[MAX_POLL_ITEMS]; int n; };
static ALooper g_looper;

static void poll_clear_outputs(int *outFd, int *outEvents, void **outData) {
  if (outFd) *outFd = 0;
  if (outEvents) *outEvents = 0;
  if (outData) *outData = NULL;
}

ALooper *ALooper_prepare(int opts) {
  (void)opts;
  return &g_looper;
}

int ALooper_addFd(ALooper *looper, int fd, int ident, int events,
                  ALooper_callbackFunc callback, void *data) {
  if (!looper) looper = &g_looper;
  mutexLock(&g_lock);
  PollItem *it = NULL;
  for (int i = 0; i < looper->n; i++)
    if (looper->items[i].fd == fd) { it = &looper->items[i]; break; }
  if (!it && looper->n < MAX_POLL_ITEMS)
    it = &looper->items[looper->n++];
  if (it) { it->fd = fd; it->ident = ident; it->events = events; it->cb = callback; it->data = data; }
  condvarWakeAll(&g_cond);
  mutexUnlock(&g_lock);
  return 1;
}

int ALooper_removeFd(ALooper *looper, int fd) {
  if (!looper) looper = &g_looper;
  mutexLock(&g_lock);
  for (int i = 0; i < looper->n; i++) {
    if (looper->items[i].fd == fd) {
      looper->items[i] = looper->items[--looper->n];
      break;
    }
  }
  mutexUnlock(&g_lock);
  return 1;
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
  // absolute deadline so unrelated wakeups don't restart a finite timeout
  const u64 deadline = (timeoutMillis > 0)
      ? armGetSystemTick() + armNsToTicks((u64)timeoutMillis * 1000000ull) : 0;
  mutexLock(&g_lock);
  for (;;) {
    for (int i = 0; i < g_looper.n; i++) {
      PollItem *it = &g_looper.items[i];
      const int slot = it->fd - FAKE_FD_BASE;
      if (slot < 0 || slot >= MAX_FAKE_FDS) continue;
      if (!fd_readable_locked(slot)) continue;
      if (it->cb) {
        ALooper_callbackFunc cb = it->cb;
        const int fd = it->fd, ev = it->events;
        void *d = it->data;
        mutexUnlock(&g_lock);
        const int keep = cb(fd, ev, d);
        if (!keep) ALooper_removeFd(&g_looper, fd);
        poll_clear_outputs(outFd, outEvents, outData);
        return ALOOPER_POLL_CALLBACK;
      }
      if (outFd) *outFd = it->fd;
      if (outEvents) *outEvents = it->events;
      if (outData) *outData = it->data;
      const int ident = it->ident;
      mutexUnlock(&g_lock);
      return ident;
    }
    if (timeoutMillis == 0) {
      mutexUnlock(&g_lock);
      poll_clear_outputs(outFd, outEvents, outData);
      return ALOOPER_POLL_TIMEOUT;
    }
    if (timeoutMillis < 0) {
      condvarWait(&g_cond, &g_lock);
    } else {
      const u64 now = armGetSystemTick();
      if (now >= deadline) {
        mutexUnlock(&g_lock);
        poll_clear_outputs(outFd, outEvents, outData);
        return ALOOPER_POLL_TIMEOUT;
      }
      const Result rc = condvarWaitTimeout(&g_cond, &g_lock, armTicksToNs(deadline - now));
      if (R_FAILED(rc)) { // timed out
        mutexUnlock(&g_lock);
        poll_clear_outputs(outFd, outEvents, outData);
        return ALOOPER_POLL_TIMEOUT;
      }
    }
  }
}

// ===========================================================================
// AInputQueue + AInputEvent  (struct definitions are near the top of the file)
// ===========================================================================

static AInputQueue g_queue;

static AInputQueue *make_input_queue(void) {
  memset(&g_queue, 0, sizeof(g_queue));
  mutexLock(&g_lock);
  const int slot = alloc_slot();
  if (slot >= 0) {
    g_fds[slot].kind = FD_INPUT;
    g_fds[slot].queue = &g_queue;
    g_queue.fd = FAKE_FD_BASE + slot;
  }
  mutexUnlock(&g_lock);
  return &g_queue;
}

void AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident,
                              ALooper_callbackFunc callback, void *data) {
  if (!queue) return;
  queue->looper = looper ? looper : &g_looper;
  queue->ident = ident;
  ALooper_addFd(queue->looper, queue->fd, ident, ALOOPER_EVENT_INPUT, callback, data);
}

void AInputQueue_detachLooper(AInputQueue *queue) {
  if (queue && queue->looper)
    ALooper_removeFd(queue->looper, queue->fd);
}

int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent) {
  if (!queue) return -1;
  mutexLock(&g_lock);
  AInputEvent *e = queue->head;
  if (!e) { mutexUnlock(&g_lock); return -1; }
  queue->head = e->next;
  if (!queue->head) queue->tail = NULL;
  queue->count--;
  mutexUnlock(&g_lock);
  e->next = NULL;
  if (outEvent) *outEvent = e;
  return 0;
}

int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event) {
  (void)queue; (void)event;
  return 0; // never pre-dispatch (we have no IME path)
}

void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled) {
  (void)queue; (void)handled;
  free(event);
}

static void enqueue_event(AInputEvent *e) {
  mutexLock(&g_lock);

  const int is_move = (e->type == AINPUT_EVENT_TYPE_MOTION &&
                       (e->action & 0xff) == AMOTION_ACTION_MOVE);
  if (is_move && g_queue.tail && g_queue.tail->type == AINPUT_EVENT_TYPE_MOTION &&
      (g_queue.tail->action & 0xff) == AMOTION_ACTION_MOVE) {
    AInputEvent *tail = g_queue.tail;
    AInputEvent *next = tail->next;
    *tail = *e;
    tail->next = next;
    free(e);
    condvarWakeAll(&g_cond);
    mutexUnlock(&g_lock);
    return;
  }

  if (is_move && g_queue.count >= INPUT_QUEUE_SOFT_CAP) {
    free(e);
    mutexUnlock(&g_lock);
    return;
  }

  e->next = NULL;
  if (g_queue.tail) g_queue.tail->next = e; else g_queue.head = e;
  g_queue.tail = e;
  g_queue.count++;
  condvarWakeAll(&g_cond);
  mutexUnlock(&g_lock);
}

void android_inject_motion(int32_t action, int pointer_count,
                           const int32_t *ids, const float *xs, const float *ys) {
  if (pointer_count > 8) pointer_count = 8;
  AInputEvent *e = calloc(1, sizeof(*e));
  if (!e) return;
  e->type = AINPUT_EVENT_TYPE_MOTION;
  e->action = action;
  e->pcount = pointer_count;
  for (int i = 0; i < pointer_count; i++) {
    e->ids[i] = ids ? ids[i] : i;
    e->xs[i] = xs ? xs[i] : 0;
    e->ys[i] = ys ? ys[i] : 0;
  }
  enqueue_event(e);
}

void android_inject_key(int32_t action, int32_t keycode) {
  AInputEvent *e = calloc(1, sizeof(*e));
  if (!e) return;
  e->type = AINPUT_EVENT_TYPE_KEY;
  e->action = action;
  e->keycode = keycode;
  enqueue_event(e);
}

// --- AInputEvent / AMotionEvent / AKeyEvent getters -------------------------

int32_t AInputEvent_getType(const AInputEvent *e)        { return e ? e->type : 0; }
int32_t AInputEvent_getDeviceId(const AInputEvent *e)    { (void)e; return 0; }
int32_t AInputEvent_getSource(const AInputEvent *e)      { (void)e; return 0; }

int32_t AMotionEvent_getAction(const AInputEvent *e)     { return e ? e->action : 0; }
size_t  AMotionEvent_getPointerCount(const AInputEvent *e) { return e ? (size_t)e->pcount : 0; }
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t i) {
  return (e && i < (size_t)e->pcount) ? e->ids[i] : 0;
}
float AMotionEvent_getX(const AInputEvent *e, size_t i) {
  return (e && i < (size_t)e->pcount) ? e->xs[i] : 0.0f;
}
float AMotionEvent_getY(const AInputEvent *e, size_t i) {
  return (e && i < (size_t)e->pcount) ? e->ys[i] : 0.0f;
}

int32_t AKeyEvent_getKeyCode(const AInputEvent *e)     { return e ? e->keycode : 0; }
int32_t AKeyEvent_getFlags(const AInputEvent *e)       { return e ? e->flags : 0; }
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e) { return e ? e->repeat : 0; }
int32_t AKeyEvent_getAction(const AInputEvent *e)      { return e ? e->action : 0; }

// ===========================================================================
// AConfiguration
// ===========================================================================

struct AConfiguration { char language[4]; char country[4]; };

static void resolve_locale(char lang[3], char country[3]) {
  // CR3 only has English + Japanese text; honour those, default the rest to en.
  if (config.language == LANG_JA) { strcpy(lang, "ja"); strcpy(country, "JP"); return; }
  if (config.language == LANG_EN) { strcpy(lang, "en"); strcpy(country, "US"); return; }
  // LANG_AUTO: Japanese system language -> Japanese, otherwise English.
  strcpy(lang, "en"); strcpy(country, "US");
  u64 code = 0;
  SetLanguage sl;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) &&
        R_SUCCEEDED(setMakeLanguage(code, &sl)) && sl == SetLanguage_JA) {
      strcpy(lang, "ja"); strcpy(country, "JP");
    }
    setExit();
  }
}

AConfiguration *AConfiguration_new(void) {
  AConfiguration *c = calloc(1, sizeof(*c));
  if (c) { strcpy(c->language, "en"); strcpy(c->country, "US"); }
  return c;
}
void AConfiguration_fromAssetManager(AConfiguration *c, AAssetManager *am) {
  (void)am;
  if (c) resolve_locale(c->language, c->country);
}
void AConfiguration_getLanguage(AConfiguration *c, char *out) {
  if (!out) return;
  out[0] = c ? c->language[0] : 'e';
  out[1] = c ? c->language[1] : 'n';
}
void AConfiguration_getCountry(AConfiguration *c, char *out) {
  if (!out) return;
  out[0] = c ? c->country[0] : 'U';
  out[1] = c ? c->country[1] : 'S';
}
void AConfiguration_delete(AConfiguration *c) { free(c); }

// ===========================================================================
// ASensor* -- never reached (the engine imports no ASensorManager), but the
// dynamic linker still needs the symbols. Keep them harmless.
// ===========================================================================

static float g_orient[3] = { 0.0f, 0.0f, 9.81f };
void android_set_orientation(float x, float y, float z) { g_orient[0] = x; g_orient[1] = y; g_orient[2] = z; }
void android_get_orientation(float *x, float *y, float *z) {
  if (x) *x = g_orient[0];
  if (y) *y = g_orient[1];
  if (z) *z = g_orient[2];
}

int  ASensorEventQueue_enableSensor(void *q, const void *s)        { (void)q; (void)s; return 0; }
int  ASensorEventQueue_disableSensor(void *q, const void *s)       { (void)q; (void)s; return 0; }
int  ASensorEventQueue_setEventRate(void *q, const void *s, int32_t us) { (void)q; (void)s; (void)us; return 0; }
int  ASensorEventQueue_getEvents(void *q, void *events, size_t count) { (void)q; (void)events; (void)count; return 0; }

// ===========================================================================
// ANativeWindow -> libnx NWindow
// ===========================================================================

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *win, int32_t w, int32_t h, int32_t format) {
  (void)format;
  if (win && w > 0 && h > 0)
    nwindowSetDimensions((NWindow *)win, (u32)w, (u32)h);
  return 0;
}

ANativeWindow *android_native_window(void) {
  NWindow *win = nwindowGetDefault();
  if (win)
    nwindowSetDimensions(win, (u32)screen_width, (u32)screen_height);
  return (ANativeWindow *)win;
}

// ===========================================================================
// host control
// ===========================================================================

static volatile int g_main_finished = 0;
int  android_main_finished(void) { return g_main_finished; }
void android_mark_main_finished(void) { g_main_finished = 1; }

void android_native_init(void) {
  if (g_inited) return;
  mutexInit(&g_lock);
  condvarInit(&g_cond);
  memset(g_fds, 0, sizeof(g_fds));
  memset(&g_looper, 0, sizeof(g_looper));
  g_inited = 1;
}

AInputQueue *android_input_queue(void) {
  return make_input_queue();
}

// the ANativeActivity is a file-scope singleton so its address stays valid for
// the lifetime of the engine thread.
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks; // the glue fills this in

ANativeActivity *android_make_activity(void *vm, void *env, void *clazz,
                                       AAssetManager *am,
                                       const char *internalPath,
                                       const char *externalPath,
                                       const char *obbPath) {
  memset(&g_activity, 0, sizeof(g_activity));
  memset(&g_callbacks, 0, sizeof(g_callbacks));
  g_activity.callbacks = &g_callbacks;
  g_activity.vm = vm;
  g_activity.env = env;
  g_activity.clazz = clazz;
  g_activity.internalDataPath = internalPath;
  g_activity.externalDataPath = externalPath;
  g_activity.obbPath = obbPath;
  g_activity.assetManager = am;
  g_activity.sdkVersion = 29; // Android 10
  return &g_activity;
}
