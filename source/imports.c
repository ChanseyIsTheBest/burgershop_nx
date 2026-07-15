/* imports.c -- .so import resolution for Burger Shop
 *               (libSexyAndroid.so = PopCap SexyAppFramework, libbass.so = BASS)
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen   (FF4:AY port these shims derive from)
 * Burger Shop port: import table + glue.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Two modules are loaded: libbass.so first, then libSexyAndroid.so. The engine
 * imports 33 BASS_* entry points; those are NOT in this table -- so_resolve
 * walks every loaded module's exports after consulting the table, so they bind
 * straight to the real libbass. Everything else (a libc subset shimmed where
 * bionic and newlib disagree, GLES1 fixed-function over mesa, OpenSL ES over
 * our opensles.c, the AAsset NDK surface, an anonymous/file mmap emulation and
 * the dynamic-linker calls BASS uses to find OpenSL) resolves from here.
 *
 * Unlike the donor's FF4 engine, the SexyAppFramework renderer is purely 2D:
 * there is no lighting/material/fog state, so none of the 3D GL fix-ups are
 * carried over -- the GL table is a flat pass-through to mesa, including the
 * GL_OES_framebuffer_object entry points the engine uses for render-to-texture.
 */

#define _GNU_SOURCE // vasprintf, strncasecmp and friends

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <EGL/egl.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "bs_shims.h"
#include "opensles.h"

extern uintptr_t __cxa_atexit;
extern uintptr_t __stack_chk_fail;

// ---------------------------------------------------------------------------
// pthread wrappers: bionic allocates the opaque sync types inline and zero-
// inits them, so we lazily back them with heap-allocated newlib objects
// stashed through the caller's pointer slot. (Carried over from the FF4 port.)
// ---------------------------------------------------------------------------

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (mutexattr && *mutexattr == 1);
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int attr = 1; ret = pthread_mutex_init_fake(uid, &attr); }
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int attr = 1; ret = pthread_mutex_init_fake(uid, &attr); }
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) ret = pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int attr = 1; ret = pthread_mutex_init_fake(uid, &attr); }
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (!*mtx && pthread_mutex_init_fake(mtx, NULL) < 0) return -1;
  return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (!*cnd && pthread_cond_init_fake(cnd, NULL) < 0) return -1;
  if (!*mtx && pthread_mutex_init_fake(mtx, NULL) < 0) return -1;
  // BASS sets its condvar clock to CLOCK_MONOTONIC and computes the deadline
  // from clock_gettime(CLOCK_MONOTONIC); newlib's condvar waits on CLOCK_REALTIME.
  // Reduce the caller's absolute deadline to a relative delay against whichever
  // clock it matches, then rebuild it as a REALTIME deadline newlib understands.
  struct timespec mono, real;
  clock_gettime(CLOCK_MONOTONIC, &mono);
  clock_gettime(CLOCK_REALTIME, &real);
  long long t_ns    = (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
  long long mono_ns = (long long)mono.tv_sec * 1000000000LL + mono.tv_nsec;
  long long real_ns = (long long)real.tv_sec * 1000000000LL + real.tv_nsec;
  long long rel_m = t_ns - mono_ns;
  long long rel_r = t_ns - real_ns;
  long long rel = (llabs(rel_m) <= llabs(rel_r)) ? rel_m : rel_r;
  if (rel < 0) rel = 0;
  if (rel > 1000000000LL) rel = 1000000000LL; // clamp to 1s, never hang
  long long deadline = real_ns + rel;
  struct timespec abs;
  abs.tv_sec  = (time_t)(deadline / 1000000000LL);
  abs.tv_nsec = (long)(deadline % 1000000000LL);
  return pthread_cond_timedwait(*cnd, *mtx, &abs);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

// engine/BASS threads must get tpidr_el0 pointed at a stack-guard block before
// they run any guarded code (see tls_setup_guard in util.c)
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  return ts.entry(ts.arg);
}

int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  (void)unused;
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  return pthread_create(thread, NULL, thread_trampoline, ts);
}

// bionic pthread_mutexattr_t is an int; we store the type plainly and read it
// back in pthread_mutex_init_fake (PTHREAD_MUTEX_RECURSIVE == 1 in bionic).
int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

// rwlock slots are also lazy void* (the rdlock/wrlock/unlock fakes in
// libc_shim.c create the libnx primitive on first use). init just clears the
// slot; destroy frees the FakeRwLock if one was created.
int pthread_rwlock_init_fake(void **rw, const void *attr) {
  (void)attr;
  if (rw) *rw = NULL;
  return 0;
}
int pthread_rwlock_destroy_fake(void **rw) {
  if (rw && *rw) { free(*rw); *rw = NULL; }
  return 0;
}

int pthread_condattr_setclock_fake(void *attr, int clock_id) { (void)attr; (void)clock_id; return 0; }

int pthread_detach_fake(pthread_t t) { (void)t; return 0; } // libnx reaps on exit
int pthread_equal_fake(unsigned long a, unsigned long b) { return a == b; }
// a stable, unique per-thread token (the thread's TLS base); good enough for
// the engine's "am I on the main thread" comparisons.
unsigned long pthread_self_fake(void) { return (unsigned long)(uintptr_t)armGetTls(); }

// ---------------------------------------------------------------------------
// small misc fakes
// ---------------------------------------------------------------------------

char *__strrchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strrchr(s, c);
}

// The engine may open the pak as an "asset" and ask for a file descriptor to
// hand to BASS. On Switch the pak is a loose file the engine also reaches via
// fopen, so we decline the fd path and let it fall back.
int AAsset_openFileDescriptor_fake(void *asset, off_t *start, off_t *len) {
  (void)asset;
  if (start) *start = 0;
  if (len) *len = 0;
  debugPrintf("io: AAsset_openFileDescriptor called -> declined (BASS will stream via read)\n");
  return -1;
}

// ---------------------------------------------------------------------------
// GL_OES_framebuffer_object (render-to-texture)
//
// devkitPro's GLES headers don't declare the *OES entry points, so we resolve
// them at runtime via eglGetProcAddress (lazily, on first use, by which point
// the context is current) and expose stable wrappers for the import table.
// If a particular mesa build only advertises the core (non-OES) names, we fall
// back to those -- same ABI.
// ---------------------------------------------------------------------------

typedef void   (*PFN_genfb)(GLsizei, GLuint *);
typedef void   (*PFN_bindfb)(GLenum, GLuint);
typedef void   (*PFN_delfb)(GLsizei, const GLuint *);
typedef GLenum (*PFN_checkfb)(GLenum);
typedef void   (*PFN_fbtex2d)(GLenum, GLenum, GLenum, GLuint, GLint);

static PFN_genfb   p_genfb;
static PFN_bindfb  p_bindfb;
static PFN_delfb   p_delfb;
static PFN_checkfb p_checkfb;
static PFN_fbtex2d p_fbtex2d;

static void resolve_fbo(void) {
  if (p_genfb) return;
  p_genfb   = (PFN_genfb)  eglGetProcAddress("glGenFramebuffersOES");
  p_bindfb  = (PFN_bindfb) eglGetProcAddress("glBindFramebufferOES");
  p_delfb   = (PFN_delfb)  eglGetProcAddress("glDeleteFramebuffersOES");
  p_checkfb = (PFN_checkfb)eglGetProcAddress("glCheckFramebufferStatusOES");
  p_fbtex2d = (PFN_fbtex2d)eglGetProcAddress("glFramebufferTexture2DOES");
  if (!p_genfb)   p_genfb   = (PFN_genfb)  eglGetProcAddress("glGenFramebuffers");
  if (!p_bindfb)  p_bindfb  = (PFN_bindfb) eglGetProcAddress("glBindFramebuffer");
  if (!p_delfb)   p_delfb   = (PFN_delfb)  eglGetProcAddress("glDeleteFramebuffers");
  if (!p_checkfb) p_checkfb = (PFN_checkfb)eglGetProcAddress("glCheckFramebufferStatus");
  if (!p_fbtex2d) p_fbtex2d = (PFN_fbtex2d)eglGetProcAddress("glFramebufferTexture2D");
}

static void w_glGenFramebuffersOES(GLsizei n, GLuint *fb) {
  resolve_fbo(); if (p_genfb) p_genfb(n, fb);
}
static void w_glBindFramebufferOES(GLenum target, GLuint fb) {
  resolve_fbo(); if (p_bindfb) p_bindfb(target, fb);
}
static void w_glDeleteFramebuffersOES(GLsizei n, const GLuint *fb) {
  resolve_fbo(); if (p_delfb) p_delfb(n, fb);
}
static GLenum w_glCheckFramebufferStatusOES(GLenum target) {
  resolve_fbo(); return p_checkfb ? p_checkfb(target) : 0;
}
static void w_glFramebufferTexture2DOES(GLenum target, GLenum attachment,
                                        GLenum textarget, GLuint texture, GLint level) {
  resolve_fbo(); if (p_fbtex2d) p_fbtex2d(target, attachment, textarget, texture, level);
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// BASS music-control diagnostic wrappers
//
// The background music (an HSTREAM) stops after ~2 minutes and never loops. To
// see how the engine drives looping (loop flag at creation, an END sync that
// restarts, or a seek), we bind these calls to loggers that forward to the real
// BASS entry points. Signatures are the public, long-stable BASS ABI.
// ---------------------------------------------------------------------------
#define BASS_SAMPLE_LOOP 4u

static uint32_t (*r_play)(uint32_t, int);
static int      (*r_stop)(uint32_t);
static uint32_t (*r_setsync)(uint32_t, uint32_t, uint64_t, void *, void *);
static int      (*r_setpos)(uint32_t, uint64_t, uint32_t);
static uint32_t (*r_flags)(uint32_t, uint32_t, uint32_t);
static uint32_t (*r_screatefile)(int, const void *, uint64_t, uint64_t, uint32_t);
static uint32_t (*r_screate)(uint32_t, uint32_t, uint32_t, void *, void *);
static uint32_t (*r_screateuser)(uint32_t, uint32_t, const void *, void *);
static uint32_t (*r_isactive)(uint32_t);

// The music (an HSTREAM, handle class 0x8000xxxx) is meant to loop, but the
// game drives looping through a playback-time END-sync callback that never
// fires under our custom OpenSL output. So we loop it ourselves: remember which
// music stream is currently playing, and if it stops on its own -- i.e. reached
// its end, as opposed to the game stopping it for a level change -- restart it.
static uint32_t s_music_handle = 0;
#define BASS_ACTIVE_STOPPED 0u

static int is_music_stream(uint32_t h) { return (h & 0xC0000000u) == 0x80000000u; }

// Called once per frame from the render loop. Cheap: one BASS_ChannelIsActive
// query, and a restart only at the instant a track ends.
void bass_music_keepalive(void) {
  if (!s_music_handle || !r_isactive) return;
  if (r_isactive(s_music_handle) == BASS_ACTIVE_STOPPED) {
    if (r_play) r_play(s_music_handle, 1);   // restart=TRUE -> seek to 0 + play
  }
}

void bass_hook_set_reals(uintptr_t play, uintptr_t stop, uintptr_t setsync,
                         uintptr_t setpos, uintptr_t flags, uintptr_t screatefile,
                         uintptr_t screate, uintptr_t screateuser, uintptr_t isactive) {
  r_play = (void *)play; r_stop = (void *)stop; r_setsync = (void *)setsync;
  r_setpos = (void *)setpos; r_flags = (void *)flags;
  r_screatefile = (void *)screatefile; r_screate = (void *)screate;
  r_screateuser = (void *)screateuser; r_isactive = (void *)isactive;
}

static uint32_t w_BASS_StreamCreateFile(int mem, const void *file, uint64_t off,
                                        uint64_t len, uint32_t flags) {
  uint32_t h = r_screatefile ? r_screatefile(mem, file, off, len, flags) : 0;
  debugPrintf("BASS_StreamCreateFile(mem=%d len=%llu flags=0x%x loop=%d) -> h=%u\n",
              mem, (unsigned long long)len, flags, (flags & BASS_SAMPLE_LOOP) != 0, h);
  return h;
}
static uint32_t w_BASS_StreamCreate(uint32_t freq, uint32_t chans, uint32_t flags,
                                    void *proc, void *user) {
  uint32_t h = r_screate ? r_screate(freq, chans, flags, proc, user) : 0;
  debugPrintf("BASS_StreamCreate(freq=%u chans=%u flags=0x%x loop=%d) -> h=%u\n",
              freq, chans, flags, (flags & BASS_SAMPLE_LOOP) != 0, h);
  return h;
}
// BASS user-file callbacks. We wrap the game's procs so we can see what BASS
// does when the music decoder hits the end: does it query length, seek back to
// 0 (a loop attempt), and does that seek succeed? That tells us why the forced
// loop flag isn't actually looping the stream.
typedef struct {
  void     (*close)(void *user);
  uint64_t (*length)(void *user);
  uint32_t (*read)(void *buffer, uint32_t length, void *user);
  int      (*seek)(uint64_t offset, void *user);
} BASS_FILEPROCS;

typedef struct { BASS_FILEPROCS orig; void *user; unsigned id; } FpCtx;

static void wf_close(void *u) {
  FpCtx *c = u; if (c->orig.close) c->orig.close(c->user);
}
static uint64_t wf_length(void *u) {
  FpCtx *c = u;
  uint64_t l = c->orig.length ? c->orig.length(c->user) : 0;
  debugPrintf("fp[%u] length -> %llu\n", c->id, (unsigned long long)l);
  return l;
}
static uint32_t wf_read(void *buf, uint32_t len, void *u) {
  FpCtx *c = u;
  uint32_t n = c->orig.read ? c->orig.read(buf, len, c->user) : 0;
  if (n == 0) debugPrintf("fp[%u] read(%u) -> EOF\n", c->id, len);
  return n;
}
static int wf_seek(uint64_t off, void *u) {
  FpCtx *c = u;
  int r = c->orig.seek ? c->orig.seek(off, c->user) : 0;
  debugPrintf("fp[%u] seek(%llu) -> %d\n", c->id, (unsigned long long)off, r);
  return r;
}
static const BASS_FILEPROCS wrap_fileprocs = { wf_close, wf_length, wf_read, wf_seek };
static unsigned g_fp_id = 0;

static uint32_t w_BASS_StreamCreateFileUser(uint32_t sys, uint32_t flags,
                                            const void *procs, void *user) {
  // Wrap the game's FILEPROC so we can trace it; leave the loop flag to the game
  // (it controls looping via an END-sync callback, promoted to MIXTIME in
  // w_BASS_ChannelSetSync so it actually fires under our output).
  FpCtx *ctx = malloc(sizeof(FpCtx));
  ctx->orig = *(const BASS_FILEPROCS *)procs; // copy: BASS doesn't keep the ptr
  ctx->user = user;
  ctx->id   = g_fp_id++;
  uint32_t h = r_screateuser ? r_screateuser(sys, flags, &wrap_fileprocs, ctx) : 0;
  debugPrintf("BASS_StreamCreateFileUser(sys=%u flags=0x%x fp=%u) -> h=%u\n",
              sys, flags, ctx->id, h);
  return h;
}
static uint32_t w_BASS_ChannelPlay(uint32_t h, int restart) {
  debugPrintf("BASS_ChannelPlay(h=%u restart=%d)\n", h, restart);
  if (is_music_stream(h)) s_music_handle = h;  // this is the track to keep looping
  return r_play ? r_play(h, restart) : 0;
}
static int w_BASS_ChannelStop(uint32_t h) {
  debugPrintf("BASS_ChannelStop(h=%u)\n", h);
  if (h == s_music_handle) s_music_handle = 0; // stopped on purpose -> don't loop it
  return r_stop ? r_stop(h) : 0;
}
static uint32_t w_BASS_ChannelSetSync(uint32_t h, uint32_t type, uint64_t param,
                                      void *proc, void *user) {
  debugPrintf("BASS_ChannelSetSync(h=%u type=0x%x param=%llu proc=%p)\n",
              h, type, (unsigned long long)param, proc);
  return r_setsync ? r_setsync(h, type, param, proc, user) : 0;
}
static int w_BASS_ChannelSetPosition(uint32_t h, uint64_t pos, uint32_t mode) {
  debugPrintf("BASS_ChannelSetPosition(h=%u pos=%llu mode=0x%x)\n",
              h, (unsigned long long)pos, mode);
  return r_setpos ? r_setpos(h, pos, mode) : 0;
}
static uint32_t w_BASS_ChannelFlags(uint32_t h, uint32_t fl, uint32_t mask) {
  // Pass through unchanged: the game manages looping via its END-sync callback
  // (now promoted to MIXTIME so it actually fires). Forcing the loop bit here
  // made tracks loop forever and broke level-to-level track switching.
  debugPrintf("BASS_ChannelFlags(h=%u flags=0x%x mask=0x%x)\n", h, fl, mask);
  return r_flags ? r_flags(h, fl, mask) : 0;
}

DynLibFunction dynlib_functions[] = {
  // BASS music-control taps (log + forward; see wrappers above)
  { "BASS_StreamCreateFile", (uintptr_t)&w_BASS_StreamCreateFile },
  { "BASS_StreamCreate", (uintptr_t)&w_BASS_StreamCreate },
  { "BASS_StreamCreateFileUser", (uintptr_t)&w_BASS_StreamCreateFileUser },
  { "BASS_ChannelPlay", (uintptr_t)&w_BASS_ChannelPlay },
  { "BASS_ChannelStop", (uintptr_t)&w_BASS_ChannelStop },
  { "BASS_ChannelSetSync", (uintptr_t)&w_BASS_ChannelSetSync },
  { "BASS_ChannelSetPosition", (uintptr_t)&w_BASS_ChannelSetPosition },
  { "BASS_ChannelFlags", (uintptr_t)&w_BASS_ChannelFlags },

  { "__sF", (uintptr_t)&fake_sF },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "abort", (uintptr_t)&abort },
  { "exit", (uintptr_t)&exit },
  { "syscall", (uintptr_t)&syscall_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "gettid", (uintptr_t)&gettid_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },

  // fortify _chk wrappers (ignore the object-size argument)
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__open_2", (uintptr_t)&open2_fake },
  { "__read_chk", (uintptr_t)&read_chk_fake },

  // AAsset NDK surface (emulated; pak is reached via fopen, see libc_shim.c)
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake },

  // memory
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "mlock", (uintptr_t)&mlock_fake },
  { "munlock", (uintptr_t)&munlock_fake },

  // mem / str
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&strlen },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strcasestr", (uintptr_t)&strcasestr_fake },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strdup", (uintptr_t)&strdup },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "strtok", (uintptr_t)&strtok },
  { "atoi", (uintptr_t)&atoi },
  { "atof", (uintptr_t)&atof },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "iswspace", (uintptr_t)&iswspace },

  // wide char
  { "wcslen", (uintptr_t)&wcslen },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },

  // printf family
  { "snprintf", (uintptr_t)&snprintf },
  { "swprintf", (uintptr_t)&swprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vasprintf", (uintptr_t)&vasprintf },

  // math
  { "acosf", (uintptr_t)&acosf },
  { "atan", (uintptr_t)&atan },
  { "atan2f", (uintptr_t)&atan2f },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "difftime", (uintptr_t)&difftime },
  { "exp", (uintptr_t)&exp },
  { "exp2", (uintptr_t)&exp2 },
  { "expf", (uintptr_t)&expf },
  { "ldexp", (uintptr_t)&ldexp },
  { "log", (uintptr_t)&log },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sinhf", (uintptr_t)&sinhf },
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "tan", (uintptr_t)&tan },

  // time
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "gettimeofday", (uintptr_t)&gettimeofday_fake },
  { "gmtime", (uintptr_t)&gmtime },
  { "localtime", (uintptr_t)&localtime },
  { "strftime", (uintptr_t)&strftime },
  { "time", (uintptr_t)&time },
  { "usleep", (uintptr_t)&usleep },

  // stdio (over the fake bionic __sF and the buffered fopen, see libc_shim.c)
  { "fopen", (uintptr_t)&fopen_fake },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fgetc", (uintptr_t)&fgetc_fake },
  { "fgets", (uintptr_t)&fgets_fake },
  { "feof", (uintptr_t)&feof_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fscanf", (uintptr_t)&fscanf_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "sscanf", (uintptr_t)&sscanf },

  // filesystem
  { "stat", (uintptr_t)&stat_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "close", (uintptr_t)&close },
  { "dup", (uintptr_t)&dup },
  { "lseek64", (uintptr_t)&lseek64_fake },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "opendir", (uintptr_t)&opendir },
  { "closedir", (uintptr_t)&closedir },
  { "readdir", (uintptr_t)&readdir_fake },
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "chdir", (uintptr_t)&chdir_fake },
  { "getcwd", (uintptr_t)&getcwd_fake },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },
  { "readlink", (uintptr_t)&readlink_fake },
  { "utime", (uintptr_t)&utime_fake },
  { "getenv", (uintptr_t)&getenv_fake },

  // pthread
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach_fake },
  { "pthread_equal", (uintptr_t)&pthread_equal_fake },
  { "pthread_self", (uintptr_t)&pthread_self_fake },
  { "pthread_sigmask", (uintptr_t)&ret0 },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_attr_init", (uintptr_t)&ret0 },
  { "pthread_attr_setdetachstate", (uintptr_t)&ret0 },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_condattr_init", (uintptr_t)&ret0 },
  { "pthread_condattr_destroy", (uintptr_t)&ret0 },
  { "pthread_condattr_setclock", (uintptr_t)&pthread_condattr_setclock_fake },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },

  // signals / non-local jumps (no real signal delivery on Switch; sigaddset is
  // not provided by the Switch newlib, so it collapses to a harmless no-op)
  { "sigaddset", (uintptr_t)&ret0 },
  { "setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },

  // process / scheduling misc
  { "getpriority", (uintptr_t)&getpriority_fake },
  { "setpriority", (uintptr_t)&setpriority_fake },
  { "system", (uintptr_t)&system_fake },

  // dynamic linker (BASS reaches OpenSL ES through these)
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dladdr", (uintptr_t)&dladdr_fake },

  // offline network stubs
  { "socket", (uintptr_t)&socket_fake },
  { "connect", (uintptr_t)&connect_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake },
  { "shutdown", (uintptr_t)&shutdown_fake },
  { "sendto", (uintptr_t)&sendto_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake },
  { "poll", (uintptr_t)&poll_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },

  // GLES1 fixed-function (mesa libGLESv1_CM) -- flat 2D pass-through
  { "glAlphaFunc", (uintptr_t)&glAlphaFunc },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glColor4x", (uintptr_t)&glColor4x },
  { "glColorPointer", (uintptr_t)&glColorPointer },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableClientState", (uintptr_t)&glDisableClientState },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableClientState", (uintptr_t)&glEnableClientState },
  { "glFinish", (uintptr_t)&glFinish },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetFloatv", (uintptr_t)&glGetFloatv },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
  { "glLoadMatrixf", (uintptr_t)&glLoadMatrixf },
  { "glMatrixMode", (uintptr_t)&glMatrixMode },
  { "glOrthof", (uintptr_t)&glOrthof },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glPopMatrix", (uintptr_t)&glPopMatrix },
  { "glPushMatrix", (uintptr_t)&glPushMatrix },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glShadeModel", (uintptr_t)&glShadeModel },
  { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
  { "glTexEnvf", (uintptr_t)&glTexEnvf },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
  { "glVertexPointer", (uintptr_t)&glVertexPointer },
  { "glViewport", (uintptr_t)&glViewport },
  // GL_OES_framebuffer_object (render-to-texture) -- resolved at runtime
  { "glBindFramebufferOES", (uintptr_t)&w_glBindFramebufferOES },
  { "glCheckFramebufferStatusOES", (uintptr_t)&w_glCheckFramebufferStatusOES },
  { "glDeleteFramebuffersOES", (uintptr_t)&w_glDeleteFramebuffersOES },
  { "glFramebufferTexture2DOES", (uintptr_t)&w_glFramebufferTexture2DOES },
  { "glGenFramebuffersOES", (uintptr_t)&w_glGenFramebuffersOES },

  // OpenSL ES (our opensles.c shim) -- engine binds BASS, BASS binds these
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  #define SL_IID(n) { "SL_IID_" #n, (uintptr_t)&SL_IID_##n }
  SL_IID(3DCOMMIT), SL_IID(3DDOPPLER), SL_IID(3DGROUPING), SL_IID(3DLOCATION),
  SL_IID(3DMACROSCOPIC), SL_IID(3DSOURCE), SL_IID(ANDROIDCONFIGURATION),
  SL_IID(ANDROIDEFFECT), SL_IID(ANDROIDEFFECTCAPABILITIES), SL_IID(ANDROIDEFFECTSEND),
  SL_IID(ANDROIDSIMPLEBUFFERQUEUE), SL_IID(AUDIODECODERCAPABILITIES), SL_IID(AUDIOENCODER),
  SL_IID(AUDIOENCODERCAPABILITIES), SL_IID(AUDIOIODEVICECAPABILITIES), SL_IID(BASSBOOST),
  SL_IID(BUFFERQUEUE), SL_IID(DEVICEVOLUME), SL_IID(DYNAMICINTERFACEMANAGEMENT),
  SL_IID(DYNAMICSOURCE), SL_IID(EFFECTSEND), SL_IID(ENGINE), SL_IID(ENGINECAPABILITIES),
  SL_IID(ENVIRONMENTALREVERB), SL_IID(EQUALIZER), SL_IID(LED), SL_IID(METADATAEXTRACTION),
  SL_IID(METADATATRAVERSAL), SL_IID(MIDIMESSAGE), SL_IID(MIDIMUTESOLO), SL_IID(MIDITEMPO),
  SL_IID(MIDITIME), SL_IID(MUTESOLO), SL_IID(NULL), SL_IID(OBJECT), SL_IID(OUTPUTMIX),
  SL_IID(PITCH), SL_IID(PLAY), SL_IID(PLAYBACKRATE), SL_IID(PREFETCHSTATUS),
  SL_IID(PRESETREVERB), SL_IID(RATEPITCH), SL_IID(RECORD), SL_IID(SEEK), SL_IID(THREADSYNC),
  SL_IID(VIBRA), SL_IID(VIRTUALIZER), SL_IID(VISUALIZATION), SL_IID(VOLUME),
  #undef SL_IID
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  // no runtime hook swaps needed for the GLES1 path
}
