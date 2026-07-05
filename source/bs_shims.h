/* bs_shims.h -- extra Bionic/NDK shims required by Burger Shop
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * libc_shim.[ch] (inherited from the FF4:AY port) already covers the large
 * common surface -- stdio over the fake __sF, fortify _chk wrappers, AAsset
 * emulation, stat/dirent layout conversion, locale, semaphores and rwlocks.
 * These are the additional entry points that libSexyAndroid.so / libbass.so
 * pull that the donor port never needed: anonymous+file mmap, the dynamic
 * linker surface BASS uses to reach OpenSL ES at runtime, offline network
 * stubs, and a handful of small libc gaps.
 */

#ifndef __BS_SHIMS_H__
#define __BS_SHIMS_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// --- memory mapping -------------------------------------------------------
void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap_fake(void *addr, size_t length);
int mlock_fake(const void *addr, size_t len);
int munlock_fake(const void *addr, size_t len);

// --- dynamic linker (BASS probes OpenSL ES through these) -----------------
void *dlopen_fake(const char *filename, int flag);
void *dlsym_fake(void *handle, const char *symbol);
int dlclose_fake(void *handle);
int dladdr_fake(const void *addr, void *info);

// --- offline network stubs ------------------------------------------------
int socket_fake(int domain, int type, int protocol);
int connect_fake(int fd, const void *addr, uint32_t len);
int setsockopt_fake(int fd, int level, int optname, const void *optval, uint32_t optlen);
int shutdown_fake(int fd, int how);
long sendto_fake(int fd, const void *buf, size_t len, int flags, const void *dst, uint32_t dlen);
long recvfrom_fake(int fd, void *buf, size_t len, int flags, void *src, uint32_t *slen);
int poll_fake(void *fds, unsigned long nfds, int timeout);
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res);
void freeaddrinfo_fake(void *res);

// --- small libc gaps ------------------------------------------------------
void *getenv_fake(const char *name);         // always NULL on Switch
int system_fake(const char *command);        // no shell -> -1
int open2_fake(const char *path, int flags); // __open_2
long read_chk_fake(int fd, void *buf, size_t nbytes, size_t buflen); // __read_chk
off_t lseek64_fake(int fd, off_t off, int whence);
int fcntl_fake(int fd, int cmd, ...);
int getpriority_fake(int which, int who);
int setpriority_fake(int which, int who, int prio);
char *getcwd_fake(char *buf, size_t size);
int chdir_fake(const char *path);
int mkdir_fake(const char *path, unsigned int mode);
int readlink_fake(const char *path, char *buf, size_t bufsiz);
int utime_fake(const char *path, const void *times);
void sincos_fake(double x, double *s, double *c);
char *strcasestr_fake(const char *haystack, const char *needle);

#endif
