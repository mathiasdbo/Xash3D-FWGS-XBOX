/*
s_dsp_gp_tu.c - one copy of the real s_dsp.c for tools/host-tests/gp_room_kernels_test.py
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Built twice (-DTU=ref, -DTU=drv) into s_dsp_gp_host.c's program: the
reference SX_RoomFX on the CPU and the GP kernel's driver (SX_R3D_GpParams)
each keep their own s_dsp.c state, cvars included, so the test can run them
side by side on the same room changes. Its exported names get the TU's
prefix; TU_control sets what the game sets.
*/
#define CAT2( a, b ) a##_##b
#define CAT( a, b ) CAT2( a, b )
#define SX_Init CAT( TU, SX_Init )
#define SX_Free CAT( TU, SX_Free )
#define SX_RoomFX CAT( TU, SX_RoomFX )
#define SX_ClearState CAT( TU, SX_ClearState )
#define SX_R3D_SetRate CAT( TU, SX_R3D_SetRate )
#define SX_R3D_GpParams CAT( TU, SX_R3D_GpParams )
#define sx_r3d_gp CAT( TU, sx_r3d_gp )
#define idsp_room CAT( TU, idsp_room )
#define cl CAT( TU, cl )

#include "s_dsp.c"

client_t cl;

/* the room (room_type), the listener in water (waterlevel 3), the table
   (dsp_coeff_table) and room_off, set as the game sets them */
void CAT( TU, control )( float room, int water, float table, float off )
{
	cl.local.waterlevel = water ? 3 : 0;
	Cvar_DirectSetValue( &room_type, room );
	Cvar_DirectSetValue( &dsp_coeff_table, table );
	room_off.value = off;
}
