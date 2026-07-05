/* config.h -- build-time constants (heap size, module/asset names, logging)
 *
 * Burger Shop (com.gobit.burgershop) Switch port.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// newlib heap cap. The engine's allocations (decoded textures, audio buffers,
// the unpacked PAK working set) all come from here via the malloc import; the
// rest of the address space backs the .so load zones for libbass +
// libSexyAndroid (see __libnx_initheap in main.c). Burger Shop's working set is
// far smaller than a 3D RPG, but SexyAppFramework keeps every decoded image
// surface resident, so keep this generous.
#define MEMORY_MB 448

// Burger Shop ships two native modules, loaded in this order (mirrors
// SexyActivity's static initializer: loadLibrary("bass"); loadLibrary("SexyAndroid")):
//   libbass.so        -- un4seen BASS audio (OpenSL ES backend)
//   libSexyAndroid.so -- PopCap SexyAppFramework (GoBit fork), the game engine
#define BASS_SO_NAME "libbass.so"
#define SO_NAME      "libSexyAndroid.so"

// The entire game payload is a single GoBit PAK ("GBPK" magic), shipped inside
// the APK as assets/pakfiletable.wmv (the .wmv extension is camouflage). Extract
// it next to the .nro. The engine parses it itself via its PakInterface; we just
// hand it the path + size (see main.c / NativeInitDirs).
#define PAK_NAME    "pakfiletable.wmv"
#define LOG_NAME    "debug.log"

// Per-line SD-card writes are very slow; set to 1 only when chasing a crash.
#define DEBUG_LOG 0

// Fixed render size. The game needs the touchscreen (handheld only), so the port
// is locked to the handheld panel; there are no user resolution options.
extern int screen_width;
extern int screen_height;

#endif
