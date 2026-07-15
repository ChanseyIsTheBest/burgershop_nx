/* main.c -- Burger Shop Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Burger Shop on Android is a PopCap/SexyAppFramework game (GoBit fork). Its
 * Java layer loaded two native modules -- libbass.so then libSexyAndroid.so --
 * and drove the engine through a GLSurfaceView: lifecycle calls on SexyActivity
 * (NativeInitJNI / NativeInitDirs / NativeInitDeviceType / NativeAppStart), GL
 * setup + per-frame calls on SexyRenderer (nativeInit / nativeResize /
 * nativeRender), and touch on SexyGLSurfaceView (SexyIOSTouchEvent). We recreate
 * that here: load + link both modules, stand up a GLES1 context and a fake JNI
 * activity, then run a libnx main loop that calls the same entry points.
 *
 * Data files (extracted from a copy of the game you own, never bundled):
 *   libbass.so, libSexyAndroid.so, pakfiletable.wmv  -- placed next to the .nro.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "libc_shim.h"
#include "nx_pointer.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "opensles.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module bass_mod; // libbass.so (loaded first)
so_module game_mod; // libSexyAndroid.so

// Replacement heap init: keep the newlib heap separate from the two .so load
// zones. newlib gets MEMORY_MB; the remainder backs the code mappings.
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
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

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = umin(size, MEMORY_MB * 1024 * 1024);
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static long check_data(void) {
  struct stat st;
  const char *files[] = { BASS_SO_NAME, SO_NAME, PAK_NAME };
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++)
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s.\nExtract it next to the .nro.", files[i]);
  if (stat(PAK_NAME, &st) < 0)
    return 0;
  return (long)st.st_size; // pak size for NativeInitDirs
}

static void set_screen_size(void) {
  // Render at 1080p in both handheld and docked. Docked shows it natively;
  // handheld renders 1080p and the compositor downscales it to the 720p panel
  // (supersampling -> sharper). The touchscreen still reports in 1280x720 panel
  // space, and nx_pointer scales those touches up into this render space, so
  // touch stays accurate; the stick/gyro/mouse cursor makes docked playable.
  screen_width  = 1920;
  screen_height = 1080;
  debugPrintf("screen mode: %dx%d\n", screen_width, screen_height);
}

// ---------------------------------------------------------------------------
// EGL / GLES1 context on the default NWindow
// ---------------------------------------------------------------------------

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, // GLES1
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num) || num < 1) {
    debugPrintf("egl: no config\n");
    return 0;
  }

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (!s_surface) { debugPrintf("egl: no surface\n"); return 0; }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY)
    return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// engine entry points (exact exported symbol names from libSexyAndroid.so)
// ---------------------------------------------------------------------------

#define SYM(name) "Java_com_gobit_sexy_" name

typedef void (*fn_v)(void *env, void *thiz);
typedef void (*fn_dirs)(void *env, void *thiz, void *apkPath, int64_t pakOffset,
                        int64_t pakSize, void *filesDir, void *cacheDir, void *extDir);
typedef void (*fn_devtype)(void *env, void *thiz, int sdk, void *brand, void *mfr,
                           void *model, void *product, void *androidId);
typedef void (*fn_appstart)(void *env, void *thiz, int w, int h, int64_t availMem,
                            float xdpi, float ydpi, void *lang, void *locale);
typedef void (*fn_resize)(void *env, void *thiz, int w, int h,
                          int insetL, int insetT, int insetR, int insetB);
typedef void (*fn_touch)(void *env, void *thiz, int action, int pointerId, float x, float y);
// SexyIOSKeyEvent(env, thiz, ch, keycode): the engine ingests typed characters
// here. arg2 carries the character (newline 0x0a handled specially), arg3 a
// keycode. We drive it to feed text from the Switch software keyboard.
typedef int  (*fn_key)(void *env, void *thiz, int ch, int keycode);

static fn_v        e_NativeInitJNI;
static fn_dirs     e_NativeInitDirs;
static fn_devtype  e_NativeInitDeviceType;
static fn_appstart e_NativeAppStart;
static fn_v        e_NativeNotifyAppEnteredBackground;
static fn_v        e_nativeInit;
static fn_resize   e_nativeResize;
static fn_v        e_nativeRender;
static fn_touch    e_SexyIOSTouchEvent;
static fn_key      e_SexyIOSKeyEvent;

// BASS's own API, called directly from our side. On Android the Java/glue layer
// drives BASS_Update() every frame to decode streams ahead into their playback
// buffers; we replaced that layer, so we must drive it ourselves or streams
// (music) fall silent once their initial buffer drains. Resolved pre-finalize.
typedef uint32_t (*fn_bass_update)(uint32_t length);
typedef uint32_t (*fn_bass_getconfig)(uint32_t option);
static fn_bass_update    e_BASS_Update = NULL;
static fn_bass_getconfig e_BASS_GetConfig = NULL;
#define BASS_CONFIG_UPDATEPERIOD  0
#define BASS_CONFIG_UPDATETHREADS 24

// The game persists settings (language, options) to registry.bin only via its
// own MyApp::WriteToRegistry, normally on a clean lifecycle shutdown. Switch
// homebrew is often hard-killed on close, so we call it directly instead --
// periodically and on focus-loss/exit -- reading the app singleton (gSexyAppBase)
// and invoking WriteToRegistry on it. This does not pause the game.
typedef void (*fn_writereg)(void *thiz);
static fn_writereg e_WriteToRegistry = NULL;
static void      **g_app_singleton   = NULL; // &Sexy::gSexyAppBase

// Full-version (trial) gate. The game stores it as SkuMgr::IsPurchased in the
// registry ("IsPurchased" key under "\SkuMgr" in registry.bin), set by Google
// Play billing after a real purchase. We read the live flag to report state; a
// genuine owner's registry.bin is what flips it (can't be faked without a buy).
typedef uint8_t (*fn_ispurchased)(void *thiz);
static fn_ispurchased e_IsPurchased    = NULL;
static void         **g_skumgr_singleton = NULL; // &Sexy::gSkuMgr

static int purchase_is_full(void) {
  if (!e_IsPurchased || !g_skumgr_singleton) return -1; // unknown
  void *sku = *g_skumgr_singleton;
  if (!sku) return -1;
  return e_IsPurchased(sku) ? 1 : 0;
}

static void save_settings(void) {
  static int n = 0;
  void *app = g_app_singleton ? *g_app_singleton : NULL;
  if (n < 4)
    debugPrintf("save_settings #%d: writereg=%p appslot=%p app=%p\n",
                n, (void *)e_WriteToRegistry, (void *)g_app_singleton, app);
  n++;
  if (e_WriteToRegistry && app)
    e_WriteToRegistry(app);
}

// --- language persistence -------------------------------------------------
// The game stores its current language as a libc++ std::string at app+0x9b0
// (from MyApp::ChangeLanguage). Its own save of that setting goes through a
// Java (JNI) path we don't have, so instead we read the live value and persist
// it to our own file, then feed it back to NativeAppStart at the next boot.
#define LANG_FILE       "language.txt"
#define APP_LANG_OFFSET 0x9b0
static char g_lang[8] = "en";

static const char *current_game_language(void) {
  if (!g_app_singleton) return NULL;
  unsigned char *app = (unsigned char *)*g_app_singleton;
  if (!app) return NULL;
  unsigned char *s = app + APP_LANG_OFFSET;
  // libc++ std::string: low bit of byte 0 clear -> SSO (data inline at s+1),
  // set -> long (data pointer at s+16). Language codes are short (SSO).
  const char *str = ((s[0] & 1) == 0) ? (const char *)(s + 1)
                                      : *(const char **)(s + 16);
  if (!str || !str[0] || strlen(str) >= sizeof(g_lang)) return NULL;
  for (const char *p = str; *p; p++)      // sanity: a lowercase code like "fr"
    if (*p < 'a' || *p > 'z') return NULL;
  return str;
}

static void persist_language(void) {
  const char *lang = current_game_language();
  if (!lang || strcmp(lang, g_lang) == 0) return; // unchanged
  strncpy(g_lang, lang, sizeof(g_lang) - 1);
  g_lang[sizeof(g_lang) - 1] = 0;
  FILE *f = fopen(LANG_FILE, "w");
  if (f) { fputs(g_lang, f); fclose(f); debugPrintf("saved language: %s\n", g_lang); }
}

static void load_saved_language(void) {
  FILE *f = fopen(LANG_FILE, "r");
  if (!f) return;
  char buf[8] = {0};
  if (fgets(buf, sizeof(buf), f)) {
    char *p = buf;
    while (*p >= 'a' && *p <= 'z') p++;
    *p = 0;
    if (buf[0]) { strncpy(g_lang, buf, sizeof(g_lang) - 1); g_lang[sizeof(g_lang) - 1] = 0; }
  }
  fclose(f);
  debugPrintf("loaded saved language: %s\n", g_lang);
}

// each manager exposes a NativeInitJNI we call once so its JNI glue is set up
// before the engine first reaches for it
static const char *manager_initjni[] = {
  SYM("AdMgr_NativeInitJNI"),
  SYM("AnalyticsMgr_NativeInitJNI"),
  SYM("CloudMgr_NativeInitJNI"),
  SYM("FacebookMgr_NativeInitJNI"),
  SYM("GameServMgr_NativeInitJNI"),
  SYM("MediaPlayerMgr_NativeInitJNI"),
  SYM("RemoteConfigMgr_NativeInitJNI"),
  SYM("SoundPoolMgr_NativeInitJNI"),
  SYM("UserDataMgr_NativeInitJNI"),
};
#define N_MANAGERS (sizeof(manager_initjni) / sizeof(manager_initjni[0]))
// resolved pointers, captured BEFORE so_finalize (after which the symbol table
// is gone). NULL for any manager this build doesn't actually export.
static fn_v e_manager_init[N_MANAGERS];

static void resolve_entry_points(void) {
  e_NativeInitJNI        = (fn_v)so_find_addr_rx(&game_mod, SYM("SexyActivity_NativeInitJNI"));
  e_NativeInitDirs       = (fn_dirs)so_find_addr_rx(&game_mod, SYM("SexyActivity_NativeInitDirs"));
  e_NativeInitDeviceType = (fn_devtype)so_find_addr_rx(&game_mod, SYM("SexyActivity_NativeInitDeviceType"));
  e_NativeAppStart       = (fn_appstart)so_find_addr_rx(&game_mod, SYM("SexyActivity_NativeAppStart"));
  e_NativeNotifyAppEnteredBackground =
      (fn_v)so_try_find_addr_rx(&game_mod, SYM("SexyActivity_NativeNotifyAppEnteredBackground"));
  e_nativeInit        = (fn_v)so_find_addr_rx(&game_mod, SYM("SexyRenderer_nativeInit"));
  e_nativeResize      = (fn_resize)so_find_addr_rx(&game_mod, SYM("SexyRenderer_nativeResize"));
  e_nativeRender      = (fn_v)so_find_addr_rx(&game_mod, SYM("SexyRenderer_nativeRender"));
  e_SexyIOSTouchEvent = (fn_touch)so_find_addr_rx(&game_mod, SYM("SexyGLSurfaceView_SexyIOSTouchEvent"));
  e_SexyIOSKeyEvent   = (fn_key)so_try_find_addr_rx(&game_mod, SYM("SexyGLSurfaceView_SexyIOSKeyEvent"));

  // managers are optional; resolve them here too, while the symbol table is
  // still readable (so_finalize, later, unmaps it)
  for (unsigned i = 0; i < N_MANAGERS; i++) {
    e_manager_init[i] = (fn_v)so_try_find_addr_rx(&game_mod, manager_initjni[i]);
    if (!e_manager_init[i])
      debugPrintf("manager init not found: %s\n", manager_initjni[i]);
  }

  // BASS's own exports (plain C names, from the bass module) — needed so we can
  // pump BASS_Update ourselves. Must be grabbed before so_finalize too.
  e_BASS_Update    = (fn_bass_update)so_try_find_addr_rx(&bass_mod, "BASS_Update");
  e_BASS_GetConfig = (fn_bass_getconfig)so_try_find_addr_rx(&bass_mod, "BASS_GetConfig");
  if (!e_BASS_Update)
    debugPrintf("BASS_Update not found -- streams may not refill\n");

  // settings persistence (registry.bin) -- resolve the writer + app singleton
  e_WriteToRegistry = (fn_writereg)so_try_find_addr_rx(&game_mod, "_ZN4Sexy5MyApp15WriteToRegistryEv");
  g_app_singleton   = (void **)so_try_find_addr_rx(&game_mod, "_ZN4Sexy12gSexyAppBaseE");
  if (!e_WriteToRegistry || !g_app_singleton)
    debugPrintf("settings save unavailable (writereg=%p app=%p)\n",
                (void *)e_WriteToRegistry, (void *)g_app_singleton);

  // full-version gate: SkuMgr::IsPurchased + the gSkuMgr singleton
  e_IsPurchased      = (fn_ispurchased)so_try_find_addr_rx(&game_mod, "_ZN4Sexy6SkuMgr11IsPurchasedEv");
  g_skumgr_singleton = (void **)so_try_find_addr_rx(&game_mod, "_ZN4Sexy7gSkuMgrE");

  // Hand the real BASS music-control pointers to the diagnostic wrappers in
  // imports.c (the engine binds those names to the wrappers via the table).
  bass_hook_set_reals(
      so_try_find_addr_rx(&bass_mod, "BASS_ChannelPlay"),
      so_try_find_addr_rx(&bass_mod, "BASS_ChannelStop"),
      so_try_find_addr_rx(&bass_mod, "BASS_ChannelSetSync"),
      so_try_find_addr_rx(&bass_mod, "BASS_ChannelSetPosition"),
      so_try_find_addr_rx(&bass_mod, "BASS_ChannelFlags"),
      so_try_find_addr_rx(&bass_mod, "BASS_StreamCreateFile"),
      so_try_find_addr_rx(&bass_mod, "BASS_StreamCreate"),
      so_try_find_addr_rx(&bass_mod, "BASS_StreamCreateFileUser"),
      so_try_find_addr_rx(&bass_mod, "BASS_ChannelIsActive"));
}

// ---------------------------------------------------------------------------
// input: the touch panel mapped into screen pixels.
//
// SexyIOSTouchEvent(action, pointerId, x, y) takes coordinates in *screen
// pixels*; the engine divides by the scale established in nativeResize. Action
// codes (verified by disassembly): 0 = down, 1 = move, 200 = up, 3 = cancel.
// ---------------------------------------------------------------------------

#define ACTION_DOWN 0
#define ACTION_MOVE 1
#define ACTION_UP   200

static void *thiz;

// All pointer input -- touchscreen, stick-driven cursor, gyro pointing, and USB
// mouse -- plus the on-screen cursor overlay are handled by the nx_pointer
// module (see nx_pointer.{c,h}). Here we just drain its events each frame and
// hand them to the engine as touch events, mapping phases to its action codes.
static void nxp_log(const char *m) { debugPrintf("%s", (char *)m); }

static void feed_pointer(void) {
  nxp_update();
  NxpEvent ev[16];
  const int n = nxp_poll(ev, 16);
  for (int i = 0; i < n; i++) {
    const int action = (ev[i].phase == NXP_DOWN) ? ACTION_DOWN
                     : (ev[i].phase == NXP_UP)   ? ACTION_UP
                     :                             ACTION_MOVE;
    e_SexyIOSTouchEvent(fake_env, thiz, action, ev[i].id, ev[i].x, ev[i].y);
  }
}

// When the game asks for the soft keyboard (JNI ShowKeyboard), pop the Switch
// system keyboard, then feed the result into the engine's focused text field the
// same way the Android IME would -- as key events via SexyIOSKeyEvent. We clear
// whatever's there first (backspaces) then type the new string + a commit.
static void handle_keyboard(void) {
  if (!g_kbd_requested)
    return;
  g_kbd_requested = 0;
  if (!e_SexyIOSKeyEvent)
    return;

  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0)))
    return;
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetStringLenMax(&kbd, 24);
  swkbdConfigSetHeaderText(&kbd, "Enter name");

  char out[64] = {0};
  Result rc = swkbdShow(&kbd, out, sizeof(out));
  swkbdClose(&kbd);
  if (R_FAILED(rc))
    return;

  for (char *p = out; *p; p++)          // strip any trailing newline
    if (*p == '\n' || *p == '\r') { *p = 0; break; }

  debugPrintf("swkbd result: \"%s\"\n", out);

  for (int i = 0; i < 32; i++)          // clear the current field
    e_SexyIOSKeyEvent(fake_env, thiz, 0x08, 0); // backspace
  for (char *p = out; *p; p++)          // type the new text
    e_SexyIOSKeyEvent(fake_env, thiz, (unsigned char)*p, 0);
  e_SexyIOSKeyEvent(fake_env, thiz, 0x0a, 0);    // enter/commit
}

// ---------------------------------------------------------------------------
// two-module load: bass first (so its exports are visible when the engine's
// 33 BASS_* imports resolve), then libSexyAndroid. Both are relocated and
// resolved before either is finalized, because cross-module symbol lookup
// reads the donor's symbol table out of its load_base, which so_finalize locks.
// ---------------------------------------------------------------------------

static void load_two_modules(void) {
  if (so_load(&bass_mod, BASS_SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", BASS_SO_NAME);

  const size_t used = ALIGN_MEM(bass_mod.load_size, 0x1000);
  void *base2 = (char *)heap_so_base + used;
  size_t avail2 = (heap_so_limit > used) ? heap_so_limit - used : 0;

  if (so_load(&game_mod, SO_NAME, base2, avail2) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  so_relocate(&bass_mod);
  so_relocate(&game_mod);

  update_imports();

  // bass first: its libc / OpenSL / dl imports come straight from the table.
  so_resolve(&bass_mod, dynlib_functions, dynlib_numfunctions, 1);
  // engine next: the table is consulted first, then sibling modules, so the
  // 33 BASS_* imports bind to the real bass module sitting in so_list.
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  // resolve our entry points while game_mod's symbol table is still readable
  resolve_entry_points();

  // now map both as executable code (this is what locks load_base)
  so_finalize(&bass_mod);
  so_finalize(&game_mod);
  so_flush_caches(&bass_mod);
  so_flush_caches(&game_mod);

  // the engine reads its stack-protector canary from tpidr_el0+0x28; set it up
  // on this (main) thread before any engine code runs
  tls_setup_guard();

  so_execute_init_array(&bass_mod); // BASS static init
  so_execute_init_array(&game_mod); // SexyAppFramework C++ constructors

  so_free_temp(&bass_mod);
  so_free_temp(&game_mod);
}

// ---------------------------------------------------------------------------
// the Android lifecycle, replayed against the fake JNI activity
// ---------------------------------------------------------------------------

static void start_engine(long pak_size) {
  // 1) JNI init for the activity, then every manager (pointers were captured
  // before so_finalize unmapped the symbol table)
  e_NativeInitJNI(fake_env, thiz);
  for (unsigned i = 0; i < N_MANAGERS; i++)
    if (e_manager_init[i])
      e_manager_init[i](fake_env, thiz);

  // 2) directories + the pak. The engine's real arg order is (apkPath, offset,
  // size): a non-zero size selects the branch that opens apkPath as a loose pak
  // (GoBit's "load debug pak file from path"); our extracted pak is a standalone
  // file, so offset = 0 and size = the file size. (Passing size = 0 takes a dead
  // "PakFile"-property branch instead.) All writable dirs point at the working
  // directory next to the .nro.
  void *apk    = jni_make_string(PAK_NAME);
  void *files  = jni_make_string(".");
  void *cache  = jni_make_string(".");
  void *ext    = jni_make_string(".");
  debugPrintf("NativeInitDirs(pak=%s offset=0 size=%ld)\n", PAK_NAME, pak_size);
  e_NativeInitDirs(fake_env, thiz, apk, 0, (int64_t)pak_size, files, cache, ext);

  // 3) device type -- present as a generic Android tablet
  void *brand   = jni_make_string("Nintendo");
  void *mfr     = jni_make_string("Nintendo");
  void *model   = jni_make_string("Switch");
  void *product = jni_make_string("Switch");
  void *andid   = jni_make_string("0000000000000000");
  e_NativeInitDeviceType(fake_env, thiz, 29, brand, mfr, model, product, andid);

  // 4) app start -- screen size, memory budget, dpi, language.
  // We hand the engine a neutral "en" as the initial language only; the game has
  // its own in-game language selector, which is authoritative from there on, so
  // there's no external language toggle to keep in sync.
  load_saved_language(); // restore the language the player last chose
  const char *lang_s = g_lang;
  char locale_s[16];
  snprintf(locale_s, sizeof(locale_s), "%s_US", lang_s);
  void *lang   = jni_make_string(lang_s);
  void *locale = jni_make_string(locale_s);
  debugPrintf("NativeAppStart(%dx%d lang=%s)\n", screen_width, screen_height, lang_s);
  e_NativeAppStart(fake_env, thiz, screen_width, screen_height,
                   (int64_t)512 * 1024 * 1024, 160.0f, 160.0f, lang, locale);

  // 5) GL: init once with the context current, then size the viewport (no notch
  // insets on Switch)
  e_nativeInit(fake_env, thiz);
  e_nativeResize(fake_env, thiz, screen_width, screen_height, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------

// System lifecycle: persist settings when the user backgrounds (HOME) or closes
// the title -- these are the moments right before the process may be suspended
// or killed. Saving here is cheap and doesn't pause the game.
static void applet_hook_fn(AppletHookType type, void *param) {
  (void)param;
  if (type == AppletHookType_OnExitRequest) {
    persist_language();
    save_settings();
    nxp_save_settings();
    jni_quit_requested = 1;
  } else if (type == AppletHookType_OnFocusState) {
    if (appletGetFocusState() != AppletFocusState_InFocus) {
      persist_language();
      save_settings();
      nxp_save_settings();
    }
  }
}

int main(void) {
  cpu_boost(1);

  // When the user closes the title from the HOME menu the system sends an exit
  // request; break the render loop cleanly on it so the shutdown path (which
  // flushes the game's settings to disk) runs instead of being killed mid-frame.
  static AppletHookCookie s_applet_cookie;
  appletHook(&s_applet_cookie, applet_hook_fn, NULL);

  check_syscalls();
  const long pak_size = check_data();
  set_screen_size();

  // SDL backs our OpenSL ES shim (audio only); main is already running
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES context.");

  // Pointer input + on-screen cursor (touch, stick cursor, gyro, USB mouse).
  // nx_pointer owns the pad/touchscreen/mouse HID, so we don't init them here.
  static NxpConfig pcfg;
  memset(&pcfg, 0, sizeof(pcfg));
  pcfg.screen_w = screen_width;  pcfg.screen_h = screen_height; // 1280x720
  pcfg.panel_w  = 1280;          pcfg.panel_h  = 720;           // Switch touch panel
  pcfg.data_dir = ".";           // cursor.png / pointer.cfg next to the .nro
  pcfg.cursor_id = 8;            // cursor pointer id (touch uses 0)
  pcfg.max_touch_slots = 8;
  pcfg.log       = nxp_log;
  pcfg.fopen_fn  = fopen_fake;   // route settings I/O through the port's fopen
  pcfg.fclose_fn = fclose;
  nxp_init(&pcfg);

  debugPrintf("heap: newlib %u MB, .so zone %u KB at %p\n",
      MEMORY_MB, (unsigned)(heap_so_limit / 1024), heap_so_base);

  load_two_modules();

  jni_init();
  thiz = jni_make_thiz();

  start_engine(pak_size);

  // Diagnostic: is BASS's automatic stream-update thread even enabled? If the
  // period is 0, BASS expects the app to call BASS_Update() itself (which we now
  // do in the loop). Log it once so the log confirms the mechanism.
  if (e_BASS_GetConfig)
    debugPrintf("BASS updateperiod=%u threads=%u\n",
                e_BASS_GetConfig(BASS_CONFIG_UPDATEPERIOD),
                e_BASS_GetConfig(BASS_CONFIG_UPDATETHREADS));

  // Report full-version vs trial. A genuine owner's registry.bin (with SkuMgr
  // IsPurchased=1) copied next to the .nro flips this to "full".
  {
    int p = purchase_is_full();
    debugPrintf("purchase state: %s\n",
                p == 1 ? "FULL" : p == 0 ? "TRIAL" : "unknown");
  }

  int frame = 0;
  int boot_frames = 0;
  u64 acc_render = 0, acc_swap = 0, max_render = 0, max_swap = 0;
  u64 win_start = armGetSystemTick();
  // Frame pacer: hold the loop to a steady 60.0 Hz. The mesa present is async
  // (eglSwapBuffers doesn't block), so without this the loop free-runs ~0.2ms
  // fast every frame and laps the panel about once a second -> visible judder.
  const u64 frame_ticks = armNsToTicks(1000000000ull / 60);
  u64 frame_deadline = armGetSystemTick() + frame_ticks;
  while (appletMainLoop() && !jni_quit_requested) {
    feed_pointer();
    handle_keyboard();

    // Decode streams ahead into their playback buffers, like the Android glue
    // did per frame. Without this, HSTREAM music goes silent once its initial
    // buffer drains (~0.5-1s) while in-memory HSAMPLE SFX keep playing. Ask for
    // a generous chunk; BASS caps it to the configured buffer, so we can't
    // over-render, and calling it alongside any auto-update thread is safe.
    if (e_BASS_Update)
      e_BASS_Update(200);
    bass_music_keepalive(); // loop the music if it ended

    const u64 t0 = armGetSystemTick();
    e_nativeRender(fake_env, thiz);
    const u64 t1 = armGetSystemTick();
    nxp_draw(); // cursor overlay on top of the finished frame
    eglSwapBuffers(s_display, s_surface);
    const u64 t2 = armGetSystemTick();

    const u64 r = armTicksToNs(t1 - t0), s = armTicksToNs(t2 - t1);
    acc_render += r; acc_swap += s;
    if (r > max_render) max_render = r;
    if (s > max_swap) max_swap = s;

    if (frame < 5 || (frame % 120) == 0) {
      const u64 win_ns = armTicksToNs(armGetSystemTick() - win_start);
      const int n = (frame % 120 == 0 && frame) ? 120 : (frame ? frame : 1);
      debugPrintf("frame %d: fps=%.1f render avg=%.1fms max=%.1fms | swap avg=%.1fms max=%.1fms\n",
                  frame, n * 1e9 / (double)(win_ns ? win_ns : 1),
                  acc_render / n / 1e6, max_render / 1e6,
                  acc_swap / n / 1e6, max_swap / 1e6);
      acc_render = acc_swap = max_render = max_swap = 0;
      win_start = armGetSystemTick();
    }

    // --- hold to 60.0 Hz -----------------------------------------------------
    {
      const s64 remain = (s64)(frame_deadline - armGetSystemTick());
      if (remain > 0) {
        const u64 remain_ns = armTicksToNs((u64)remain);
        // sleep the bulk (cheap, low power), busy-wait only the sub-ms tail for
        // precision the scheduler can't give us
        if (remain_ns > 1500000ull)
          svcSleepThread((s64)(remain_ns - 1000000ull));
        while ((s64)(frame_deadline - armGetSystemTick()) > 0)
          ; // spin the last <=1ms
        frame_deadline += frame_ticks;
      } else {
        // we fell behind (a genuine spike) — resync so we don't try to "catch
        // up" by racing several frames, which would itself look like judder
        frame_deadline = armGetSystemTick() + frame_ticks;
      }
    }
    frame++;

    // Persist the language shortly after the player changes it (checked cheaply
    // every ~2s; only writes on an actual change).
    if (frame % 120 == 0)
      persist_language();
    // Safety net for hard closes: flush settings to disk periodically (~30s) so
    // a change like language survives even if the process is killed abruptly.
    if (frame % 1800 == 0)
      save_settings();

    if (boot_frames < 10 && ++boot_frames == 10)
      cpu_boost(0); // drop the boot clock boost once we're past init
  }

  debugPrintf("shutting down\n");
  // Persist settings (language, options) one last time on the way out. We call
  // the game's own registry writer directly rather than relying on the lifecycle
  // shutdown path, which Switch homebrew often doesn't get to run.
  persist_language();
  save_settings();
  nxp_save_settings(); // flush any pending pointer.cfg write
  if (e_NativeNotifyAppEnteredBackground)
    e_NativeNotifyAppEnteredBackground(fake_env, thiz);
  debugLogFlush();

  opensles_shutdown();
  egl_deinit();
  debugLogFlush();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
