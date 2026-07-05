/* obb.h -- OBB asset reader interface (stub for Burger Shop)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Unlike the FF4:AY donor port, Burger Shop ships no encrypted Square-Enix
 * .obb archive: all of its data lives in a single GoBit "GBPK" pak file
 * (assets/pakfiletable.wmv) which the PopCap/SexyAppFramework engine opens
 * directly via fopen() through its own PakInterface. There is therefore no
 * OBB to decrypt, and these entry points are inert stubs that report "no
 * archive present", which makes the AAsset emulation in libc_shim.c fall
 * straight through to loose-file lookups.
 */

#ifndef __OBB_H__
#define __OBB_H__

#include <stddef.h>

// No OBB for this title: always reports failure / empty.
int obb_open(const char *path);
void obb_close(void);
int obb_exists(const char *name);
void *obb_read(const char *name, size_t *out_size);

#endif
