/* main.c -- Chaos Rings 3 Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * The MVGL engine is a NativeActivity app: we load libc++_shared.so + libcrx.so,
 * hand the engine an ANativeActivity, and let its android_main() thread own the
 * render loop. This (UI) thread drives the activity lifecycle and feeds Switch
 * HID into the engine's AInputQueue.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include <math.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "data.h"
#include "text2bitmap.h"
#include "android_native.h"
#include "opensles.h"
#include "movie_player.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module cpp_mod;   // libc++_shared.so
so_module game_mod;  // libcrx.so

// reserve a fixed slice of the address space for the two .so images and give
// everything else to the newlib heap the engine mallocs from.
#define SO_REGION_BYTES (96u * 1024 * 1024)

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_region = SO_REGION_BYTES;
  if (so_region > size / 2)
    so_region = size / 2;
  size_t fake_heap_size = size - so_region;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  const char *files[] = { SO_NAME, SO_CPP_NAME, MAIN_MVGL };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++) {
    if (stat(data_path(files[i], path, sizeof(path)), &st) < 0)
      fatal_error("Could not find\n%s.\nCheck your data files.", files[i]);
  }
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
}

// ---------------------------------------------------------------------------
// load a module from the game directory, relocate + resolve its imports
// ---------------------------------------------------------------------------
static int load_module(so_module *mod, const char *name) {
  char path[768];
  data_path(name, path, sizeof(path));
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  // advance the .so arena so the next module lands after this one
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  crx_resolve_imports(mod);
  return 0;
}

// ---------------------------------------------------------------------------
// input: Switch HID -> AInputQueue motion/key events
// ---------------------------------------------------------------------------

#define AMOTION_ACTION_DOWN 0
#define AMOTION_ACTION_UP   1
#define AMOTION_ACTION_MOVE 2
#define AKEY_ACTION_DOWN 0
#define AKEY_ACTION_UP   1
#define AKEYCODE_BACK 4

static PadState pad;
static int back_held = 0;

// --- multi-touch injection -------------------------------------------------
// Reconcile virtual fingers (each with a stable pointer id) into Android motion
// events: DOWN/POINTER_DOWN on press, POINTER_UP/UP on release, one MOVE/frame.
#define AMOTION_ACTION_POINTER_DOWN 5
#define AMOTION_ACTION_POINTER_UP   6
#define VF_MAX 4
static struct { int fid; float x, y; } g_slots[VF_MAX];
static int g_nslots = 0;

static int vf_idx(int fid) {
  for (int i = 0; i < g_nslots; i++) if (g_slots[i].fid == fid) return i;
  return -1;
}
static void vf_emit(int action) {
  if (g_nslots <= 0) return;
  int32_t ids[VF_MAX]; float xs[VF_MAX], ys[VF_MAX];
  for (int i = 0; i < g_nslots; i++) { ids[i] = g_slots[i].fid; xs[i] = g_slots[i].x; ys[i] = g_slots[i].y; }
  android_inject_motion(action, g_nslots, ids, xs, ys);
}
static void vf_set(int fid, int active, float x, float y) {
  const int idx = vf_idx(fid);
  if (active) {
    if (idx < 0) {                                  // press
      g_slots[g_nslots].fid = fid; g_slots[g_nslots].x = x; g_slots[g_nslots].y = y;
      g_nslots++;
      vf_emit(g_nslots == 1 ? AMOTION_ACTION_DOWN
                            : (AMOTION_ACTION_POINTER_DOWN | ((g_nslots - 1) << 8)));
    } else {                                        // hold: update position
      g_slots[idx].x = x; g_slots[idx].y = y;
    }
  } else if (idx >= 0) {                            // release at current position
    vf_emit(g_nslots == 1 ? AMOTION_ACTION_UP
                          : (AMOTION_ACTION_POINTER_UP | (idx << 8)));
    for (int i = idx; i < g_nslots - 1; i++) g_slots[i] = g_slots[i + 1];
    g_nslots--;
  }
}

#define STICK_DZ 4500.0f   // analog deadzone (~14%)

// Stick -> drag offset: zero inside the radial deadzone, then ramping smoothly to
// rad at full deflection. Returns 1 when active. Screen Y is down-positive.
static int stick_drag(int sx, int sy, float rad, float *dx, float *dy) {
  const float fx = (float)sx, fy = (float)sy;
  const float mag = sqrtf(fx * fx + fy * fy);
  if (mag < STICK_DZ) { *dx = *dy = 0.0f; return 0; }
  float s = (mag - STICK_DZ) / (32767.0f - STICK_DZ);
  if (s > 1.0f) s = 1.0f;
  *dx =  (fx / mag) * s * rad;
  *dy = -(fy / mag) * s * rad;
  return 1;
}

// Drive one floating-joystick finger from a stick. The finger snaps back to its
// origin when the stick centres and only lifts after a sustained release (a quick
// lift+repress would otherwise strand the joystick at the last deflection).
typedef struct { int down, rel; } Stick;
static void drive_stick(int fid, HidAnalogStickState st, float ox, float oy, float rad, Stick *s) {
  float dx, dy;
  if (stick_drag(st.x, st.y, rad, &dx, &dy)) {
    if (!s->down) { vf_set(fid, 1, ox, oy); s->down = 1; }
    vf_set(fid, 1, ox + dx, oy + dy);
    s->rel = 0;
  } else if (s->down) {
    vf_set(fid, 1, ox, oy);
    if (++s->rel > 12) { vf_set(fid, 0, 0, 0); s->down = 0; }
  }
}

static void update_input(void) {
  padUpdate(&pad);
  const u64 down = padGetButtonsDown(&pad);
  const u64 held = padGetButtons(&pad);

  const float sw = (float)screen_width, sh = (float)screen_height;

  // Left stick -> movement (lower-left), right stick -> camera (lower-right). The
  // engine routes each touch to move/camera by screen side, so both run at once.
  static Stick mv, cam;
  drive_stick(0, padGetStickPos(&pad, 0), sw * 0.24f, sh * 0.62f, sh * 0.16f, &mv);
  drive_stick(1, padGetStickPos(&pad, 1), sw * 0.76f, sh * 0.62f, sh * 0.18f, &cam);

  // A -> a quick tap at screen centre (advance dialogue / confirm).
  static int a_tap = 0;
  if (down & HidNpadButton_A) a_tap = 4;
  if (a_tap > 0) { vf_set(2, a_tap > 1, sw * 0.5f, sh * 0.5f); a_tap--; }

  // Touchscreen passthrough for handheld menu taps.
  HidTouchScreenState ts = {0};
  if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0)
    vf_set(3, 1, ts.touches[0].x * (sw / 1280.0f), ts.touches[0].y * (sh / 720.0f));
  else
    vf_set(3, 0, 0, 0);

  static u64 last_move_tick = 0;
  const u64 now = armGetSystemTick();
  if (g_nslots > 0) {
    if (!last_move_tick ||
        armTicksToNs(now - last_move_tick) >= 16666667ull) {
      vf_emit(AMOTION_ACTION_MOVE);
      last_move_tick = now;
    }
  } else {
    last_move_tick = 0;
  }

  // --- B / + map to the Android BACK key (cancel / menu back) ---
  if ((down & HidNpadButton_B) || (down & HidNpadButton_Plus)) {
    if (!back_held) { back_held = 1; android_inject_key(AKEY_ACTION_DOWN, AKEYCODE_BACK); }
  }
  if (back_held && !(held & (HidNpadButton_B | HidNpadButton_Plus))) {
    back_held = 0;
    android_inject_key(AKEY_ACTION_UP, AKEYCODE_BACK);
  }
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  cpu_boost(1);

  data_init(argc > 0 ? argv[0] : NULL);
  chdir(data_dir());
#if DEBUG_LOG
  unlink(LOG_NAME); // start each run with a fresh debug.log (no stale sessions)
#endif
  debugPrintf("=== chaosring3_nx boot === gamedir=%s\n", data_dir());

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);
  debugPrintf("[boot] config ok, screen %dx%d lang=%d\n", screen_width, screen_height, config.language);

  plInitialize(PlServiceType_User);
  text2bitmap_init();
  debugPrintf("[boot] plInitialize + text2bitmap_init done\n");

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());

  // load the C++ runtime first so libcrx's std::/operator-new imports resolve
  if (load_module(&cpp_mod, SO_CPP_NAME) < 0)
    fatal_error("Could not load\n%s.", SO_CPP_NAME);
  debugPrintf("[boot] loaded %s\n", SO_CPP_NAME);
  if (load_module(&game_mod, SO_NAME) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);
  debugPrintf("[boot] loaded %s\n", SO_NAME);

  ANativeActivity_createFunc *onCreate =
      (ANativeActivity_createFunc *)so_find_addr_rx(&game_mod, "ANativeActivity_onCreate");
  if (!onCreate)
    fatal_error("Could not resolve ANativeActivity_onCreate.");
  debugPrintf("[boot] resolved ANativeActivity_onCreate=%p\n", (void *)onCreate);

  // Resolve the engine's Fios reader (for FMV) here, while libcrx's symbol table
  // is still mapped -- so_finalize remaps load_base as code and the table goes
  // away after it. The returned addresses are load_virtbase-relative, so they
  // stay valid post-finalize.
  movie_player_init();

  so_finalize(&cpp_mod);  so_flush_caches(&cpp_mod);
  so_finalize(&game_mod); so_flush_caches(&game_mod);
  debugPrintf("[boot] modules finalized (mapped as code)\n");

  tls_setup_guard();

  so_execute_init_array(&cpp_mod);  // libc++ static init first
  debugPrintf("[boot] libc++ init_array done\n");
  so_execute_init_array(&game_mod); // engine C++ constructors
  debugPrintf("[boot] libcrx init_array done\n");
  so_free_temp(&cpp_mod);
  so_free_temp(&game_mod);

  jni_init();
  android_native_init();
  debugPrintf("[boot] jni + native host init done\n");

  void *asset_mgr = AAssetManager_fromJava(fake_env, NULL);
  ANativeActivity *act = android_make_activity(
      fake_vm, fake_env, jni_make_activity_object(),
      (AAssetManager *)asset_mgr, data_dir(), data_dir(), data_dir());

  // spawns the android_main() thread (engine owns the render loop)
  debugPrintf("[boot] calling ANativeActivity_onCreate...\n");
  onCreate(act, NULL, 0);
  debugPrintf("[boot] onCreate returned, callbacks=%p\n", (void *)act->callbacks);

  // drive the activity lifecycle: the glue forwards each callback to the engine
  // thread over its command pipe (handled by our ALooper).
  ANativeActivityCallbacks *cb = act->callbacks;
  AInputQueue *queue = android_input_queue();
  ANativeWindow *window = android_native_window();

  if (cb->onStart)  cb->onStart(act);
  debugPrintf("[boot] onStart done\n");
  if (cb->onResume) cb->onResume(act);
  debugPrintf("[boot] onResume done\n");
  if (cb->onInputQueueCreated)   cb->onInputQueueCreated(act, queue);
  debugPrintf("[boot] onInputQueueCreated done\n");
  if (cb->onNativeWindowCreated) cb->onNativeWindowCreated(act, window);
  debugPrintf("[boot] onNativeWindowCreated done\n");
  if (cb->onWindowFocusChanged)  cb->onWindowFocusChanged(act, 1);
  debugPrintf("[boot] onWindowFocusChanged done -> entering main loop\n");

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  int boot = 0;
  while (appletMainLoop() && !jni_quit_requested && !android_main_finished()) {
    update_input();
    if (boot < 120 && ++boot == 120) cpu_boost(0); // drop the load-time CPU boost
    svcSleepThread(4 * 1000000ull); // ~4 ms; the engine paces its own frames
  }

  // teardown
  if (cb->onWindowFocusChanged)   cb->onWindowFocusChanged(act, 0);
  if (cb->onNativeWindowDestroyed) cb->onNativeWindowDestroyed(act, window);
  if (cb->onInputQueueDestroyed)  cb->onInputQueueDestroyed(act, queue);
  if (cb->onPause)   cb->onPause(act);
  if (cb->onStop)    cb->onStop(act);
  if (cb->onDestroy) cb->onDestroy(act);

  opensles_shutdown();
  plExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
