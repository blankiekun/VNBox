/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Host wined3d thunk (BOXEDWINE_HOST_WINED3D): the guest wined3d.dll stub's int 0x99 calls in
// [WD3D_CALL_BASE, WD3D_CALL_LIMIT) land here (glcommon.cpp callOpenGL). Sets EAX.
#ifndef HOSTWD3D_THUNKHOST_H
#define HOSTWD3D_THUNKHOST_H

#define HOSTWD3D_CALL_BASE 0x00f00000u  // = WD3D_CALL_BASE (thunk_ids.h)
#define HOSTWD3D_CALL_LIMIT 0x00f10000u // = WD3D_CALL_LIMIT

class CPU;
void hostWined3dCall(CPU* cpu, U32 index);
void hostWined3dStatsLine(char* out, int len, double sec); // stats=1: " | wd3d calls/s=..." for this window, then resets

#endif
