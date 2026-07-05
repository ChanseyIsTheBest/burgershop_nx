/* imports.h -- .so import resolution interface
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

void update_imports(void);

// Diagnostic wrappers around the engine's BASS music-control calls log the call
// then forward to the real BASS entry point. main.c resolves the real pointers
// from the bass module (pre-finalize) via this setter.
void bass_hook_set_reals(
    uintptr_t play, uintptr_t stop, uintptr_t setsync, uintptr_t setpos,
    uintptr_t flags, uintptr_t screatefile, uintptr_t screate, uintptr_t screateuser);

#endif
