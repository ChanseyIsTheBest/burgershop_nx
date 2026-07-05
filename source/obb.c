/* obb.c -- OBB asset reader (stub for Burger Shop)
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen (original FF4:AY implementation)
 * Stubbed for Burger Shop, which has no encrypted OBB archive.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stddef.h>
#include "obb.h"

int obb_open(const char *path) {
  (void)path;
  return -1; // no archive
}

void obb_close(void) {
}

int obb_exists(const char *name) {
  (void)name;
  return 0;
}

void *obb_read(const char *name, size_t *out_size) {
  (void)name;
  if (out_size)
    *out_size = 0;
  return NULL; // fall through to loose-file / pak lookups
}
