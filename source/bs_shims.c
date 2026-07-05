/* bs_shims.c -- extra Bionic/NDK shims required by Burger Shop
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * See bs_shims.h for the rationale. Everything here is deliberately small and
 * defensive: the goal is to let libSexyAndroid.so and libbass.so load and run
 * on Switch even though Horizon offers no Linux mmap, no dynamic loader for
 * Android .so names, and no sockets in the way Android exposes them. The game
 * is fully offline and self-contained, so the network surface is stubbed and
 * mmap is emulated over aligned malloc + seek-read.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "util.h"
#include "so_util.h"
#include "opensles.h"
#include "libc_shim.h"
#include "bs_shims.h"

// Bionic/Linux mmap constants (independent of the host newlib values)
#define BIONIC_PROT_NONE   0x0
#define BIONIC_MAP_SHARED  0x1
#define BIONIC_MAP_PRIVATE 0x2
#define BIONIC_MAP_FIXED   0x10
#define BIONIC_MAP_ANON    0x20
#define BIONIC_MAP_FAILED  ((void *)-1)

// ---------------------------------------------------------------------------
// mmap/munmap emulation
//
// Horizon has no demand-paged file mapping, so we satisfy every request with
// a heap allocation. Anonymous maps are just zeroed memory; file-backed maps
// are filled once via pread at the requested offset. A small table remembers
// each base pointer so munmap can free it (engine/BASS pass the exact base
// back). MAP_FIXED is honoured only in the degenerate addr==NULL case.
// ---------------------------------------------------------------------------

typedef struct {
  void *base;
  size_t len;
} MmapRec;

static MmapRec *g_maps = NULL;
static size_t g_maps_len = 0, g_maps_cap = 0;
static Mutex g_maps_mtx;
static bool g_maps_mtx_init = false;

static void maps_lock(void) {
  if (!g_maps_mtx_init) { mutexInit(&g_maps_mtx); g_maps_mtx_init = true; }
  mutexLock(&g_maps_mtx);
}
static void maps_unlock(void) { mutexUnlock(&g_maps_mtx); }

static void maps_remember(void *base, size_t len) {
  maps_lock();
  if (g_maps_len == g_maps_cap) {
    size_t ncap = g_maps_cap ? g_maps_cap * 2 : 16;
    MmapRec *n = realloc(g_maps, ncap * sizeof(*n));
    if (n) { g_maps = n; g_maps_cap = ncap; }
  }
  if (g_maps_len < g_maps_cap) {
    g_maps[g_maps_len].base = base;
    g_maps[g_maps_len].len = len;
    g_maps_len++;
  }
  maps_unlock();
}

static int maps_forget(void *base) {
  int found = 0;
  maps_lock();
  for (size_t i = 0; i < g_maps_len; i++) {
    if (g_maps[i].base == base) {
      g_maps[i] = g_maps[g_maps_len - 1];
      g_maps_len--;
      found = 1;
      break;
    }
  }
  maps_unlock();
  return found;
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  (void)addr; (void)prot;
  if (length == 0)
    return BIONIC_MAP_FAILED;

  // 64-byte alignment keeps SIMD copies and BASS buffers happy
  void *mem = NULL;
  if (posix_memalign_fake(&mem, 64, length) != 0 || !mem)
    return BIONIC_MAP_FAILED;
  memset(mem, 0, length);

  if (!(flags & BIONIC_MAP_ANON) && fd >= 0) {
    // file-backed: pull the requested window in once. newlib has no pread, so
    // seek-read-restore by hand (mmap callers don't expect the fd position to
    // move).
    const off_t saved = lseek(fd, 0, SEEK_CUR);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    if (lseek(fd, offset, SEEK_SET) == offset) {
      ssize_t got = read(fd, mem, length);
      if (got < 0)
        debugPrintf("mmap: read(fd=%d,len=%zu,off=%lld) failed\n",
                    fd, length, (long long)offset);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double dt = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1.0e6;
    debugPrintf("io: mmap file-backed fd=%d len=%zu took %.1f ms\n", fd, length, dt);
    if (saved >= 0)
      lseek(fd, saved, SEEK_SET);
  }

  maps_remember(mem, length);
  return mem;
}

int munmap_fake(void *addr, size_t length) {
  (void)length;
  if (addr && addr != BIONIC_MAP_FAILED && maps_forget(addr))
    free(addr);
  return 0;
}

int mlock_fake(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int munlock_fake(const void *addr, size_t len) { (void)addr; (void)len; return 0; }

// ---------------------------------------------------------------------------
// dynamic linker surface
//
// BASS reaches OpenSL ES the Android way: dlopen("libOpenSLES.so") then
// dlsym(handle, "slCreateEngine"). We hand back a sentinel handle and resolve
// the one entry point BASS needs from our opensles.c shim. Any other library
// name resolves to NULL, which BASS treats as "feature unavailable" and skips
// gracefully (it never hard-depends on optional codecs here). As a safety net,
// unknown symbols are looked up across the already-loaded .so modules.
// ---------------------------------------------------------------------------

static int g_opensl_handle;  // address used purely as a unique sentinel
static int g_self_handle;

void *dlopen_fake(const char *filename, int flag) {
  (void)flag;
  if (!filename)
    return &g_self_handle; // dlopen(NULL) -> "this program"
  if (strstr(filename, "OpenSLES"))
    return &g_opensl_handle;
  debugPrintf("dlopen(%s) -> unavailable (stubbed)\n", filename);
  return NULL;
}

void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol)
    return NULL;
  if (strcmp(symbol, "slCreateEngine") == 0)
    return (void *)&slCreateEngine;

  // OpenSL interface IDs, in case BASS resolves them dynamically
  #define SL_IID_CASE(n) if (strcmp(symbol, "SL_IID_" #n) == 0) return &SL_IID_##n
  SL_IID_CASE(ENGINE);
  SL_IID_CASE(PLAY);
  SL_IID_CASE(VOLUME);
  SL_IID_CASE(RECORD);
  SL_IID_CASE(ANDROIDCONFIGURATION);
  SL_IID_CASE(ANDROIDSIMPLEBUFFERQUEUE);
  SL_IID_CASE(BUFFERQUEUE);
  SL_IID_CASE(OBJECT);
  SL_IID_CASE(OUTPUTMIX);
  #undef SL_IID_CASE

  // last resort: a real export from one of the loaded modules
  uintptr_t a = so_find_addr_in_loaded(symbol);
  if (a)
    return (void *)a;

  debugPrintf("dlsym(%s) -> NULL\n", symbol);
  return NULL;
}

int dlclose_fake(void *handle) { (void)handle; return 0; }

int dladdr_fake(const void *addr, void *info) {
  (void)addr; (void)info;
  return 0; // 0 == failure for dladdr; callers use it only for diagnostics
}

// ---------------------------------------------------------------------------
// offline network stubs -- the game never needs the network on Switch
// ---------------------------------------------------------------------------

int socket_fake(int domain, int type, int protocol) {
  (void)domain; (void)type; (void)protocol; errno = EAFNOSUPPORT; return -1;
}
int connect_fake(int fd, const void *addr, uint32_t len) {
  (void)fd; (void)addr; (void)len; errno = ENETUNREACH; return -1;
}
int setsockopt_fake(int fd, int level, int optname, const void *optval, uint32_t optlen) {
  (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; return 0;
}
int shutdown_fake(int fd, int how) { (void)fd; (void)how; return 0; }
long sendto_fake(int fd, const void *buf, size_t len, int flags, const void *dst, uint32_t dlen) {
  (void)fd; (void)buf; (void)len; (void)flags; (void)dst; (void)dlen; errno = ENETUNREACH; return -1;
}
long recvfrom_fake(int fd, void *buf, size_t len, int flags, void *src, uint32_t *slen) {
  (void)fd; (void)buf; (void)len; (void)flags; (void)src; (void)slen; errno = ENETUNREACH; return -1;
}
int poll_fake(void *fds, unsigned long nfds, int timeout) {
  (void)fds; (void)nfds;
  if (timeout > 0) svcSleepThread((int64_t)timeout * 1000000LL);
  return 0; // nothing ever ready
}
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res) {
  (void)node; (void)service; (void)hints;
  if (res) *res = NULL;
  return -2; // EAI_NONAME-ish; any non-zero is "lookup failed"
}
void freeaddrinfo_fake(void *res) { (void)res; }

// ---------------------------------------------------------------------------
// small libc gaps
// ---------------------------------------------------------------------------

void *getenv_fake(const char *name) { (void)name; return NULL; }

int system_fake(const char *command) { (void)command; return -1; }

int open2_fake(const char *path, int flags) {
  return open_fake(path, flags); // reuse the Bionic->newlib flag conversion
}

long read_chk_fake(int fd, void *buf, size_t nbytes, size_t buflen) {
  if (nbytes > buflen) nbytes = buflen; // honour the fortify bound
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  long r = read(fd, buf, nbytes);
  clock_gettime(CLOCK_MONOTONIC, &b);
  double dt = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1.0e6;
  if (dt > 40.0)
    debugPrintf("io: SLOW read(fd=%d) %zu bytes took %.1f ms\n", fd, nbytes, dt);
  return r;
}

off_t lseek64_fake(int fd, off_t off, int whence) {
  return lseek(fd, off, whence); // off_t is already 64-bit on devkitA64
}

int fcntl_fake(int fd, int cmd, ...) {
  // F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL come from <fcntl.h>; we only need
  // the handful BASS uses. (Bionic's values match newlib's here.)
  va_list ap;
  va_start(ap, cmd);
  int ret = 0;
  switch (cmd) {
    case F_DUPFD: ret = dup(fd); break;
    case F_GETFL: ret = O_RDWR; break;
    case F_GETFD:
    case F_SETFD:
    case F_SETFL: ret = 0; break;
    default:      ret = 0; break;
  }
  va_end(ap);
  return ret;
}

int getpriority_fake(int which, int who) { (void)which; (void)who; return 0; }
int setpriority_fake(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }

char *getcwd_fake(char *buf, size_t size) {
  // The engine receives all of its real paths through NativeInitDirs; cwd is
  // only used to build a few relative fallbacks, so root is a safe answer.
  if (!buf || size == 0) return NULL;
  buf[0] = '/';
  if (size > 1) buf[1] = '\0';
  else buf[0] = '\0';
  return buf;
}

int chdir_fake(const char *path) { (void)path; return 0; }

int mkdir_fake(const char *path, unsigned int mode) {
  if (mkdir(path, mode) == 0)
    return 0;
  if (errno == EEXIST)
    return 0; // already there is success for the engine's save-dir creation
  return -1;
}

int readlink_fake(const char *path, char *buf, size_t bufsiz) {
  (void)path; (void)buf; (void)bufsiz; errno = EINVAL; return -1;
}

int utime_fake(const char *path, const void *times) {
  (void)path; (void)times; return 0; // timestamps are cosmetic here
}

void sincos_fake(double x, double *s, double *c) {
  if (s) *s = sin(x);
  if (c) *c = cos(x);
}

char *strcasestr_fake(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  for (; *haystack; haystack++) {
    const char *h = haystack, *n = needle;
    while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
      h++; n++;
    }
    if (!*n)
      return (char *)haystack;
  }
  return NULL;
}
