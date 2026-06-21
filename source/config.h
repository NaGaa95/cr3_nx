/* config.h -- Chaos Rings 3 Switch wrapper configuration
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// The engine + libc++ + huge mvgl page cache need a generous newlib heap; the
// rest of system memory is handed to the .so loader (see __libnx_initheap).
#define MEMORY_MB 768

// Chaos Rings 3 (Android, com.square_enix.chaosrings3gp v1.1.4) ships the real
// engine as libcrx.so (Media.Vision "MVGL"). Unlike FF4 it is a NativeActivity
// game (android_main / ANativeActivity_onCreate) and pulls its C++ runtime from
// libc++_shared.so, so the wrapper loads BOTH shared objects.
#define SO_NAME      "libcrx.so"
#define SO_CPP_NAME  "libc++_shared.so"

// the main game archive (an APK asset). The "10007" is the APK versionCode.
#define MAIN_MVGL    "main.10007.android.mvgl"

#define CONFIG_NAME "config.txt"
#define LOG_NAME    "debug.log"

// flip to 1 (and build) to get file logging (debug.log) for on-hardware debugging
#define DEBUG_LOG 0

extern int screen_width;
extern int screen_height;

// Language. CR3 (Google Play) only has English + Japanese text; any other value
// falls back to English. 0 = follow the Switch system language (Japanese -> ja,
// otherwise en).
#define LANG_AUTO 0
#define LANG_JA   1
#define LANG_EN   2

typedef struct {
  int screen_width;
  int screen_height;
  int language;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
