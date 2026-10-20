/*
s_dsp_gp.h - the room effect's parameters for the APU's GP kernel
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
#ifndef S_DSP_GP_H
#define S_DSP_GP_H

// #180: the room effect on the APU's GP (engine/platform/xbox/apu/gp_hldsp.asm).
// SX_R3D_GpParams replaces SX_RoomFX while sx_r3d_gp is set: it runs the presets
// and cvars as SX_RoomFX would and returns what the kernel needs, which does the
// delay lines and the per-sample work. The words are the kernel's parameters.
enum
{
	SXGP_RUN,      // 1: process (SX_RoomFX would run); 0: the room mix passes untouched
	SXGP_GAIN,     // the master volume, 0..256 (S_MixBufferWithGain)
	SXGP_ALPHA,    // dsp_coeff_table 1: the reverb's sum divided by 6
	SXGP_AMOD_LP,  // room_lp != 0
	SXGP_AMOD_MOD, // room_mod != 0
	SXGP_RVB_GEN,  // +1 each time room_size is applied (RVB_CheckNewReverbVal)
	SXGP_RVB_S1,   // its two delays in samples, 0: off
	SXGP_RVB_S2,
	SXGP_RVB_LP,   // room_rvblp, as the int s_dsp.c keeps
	SXGP_RVB_FB,   // 255 * room_refl
	SXGP_DLY_GEN,  // +1 each time room_delay is applied (DLY_CheckNewDelayVal)
	SXGP_DLY_S,    // the mono delay in samples, 0: off
	SXGP_DLY_LP,   // room_dlylp
	SXGP_DLY_FB,   // 255 * room_feedback
	SXGP_STE_GEN,  // +1 each time room_left is applied (DLY_CheckNewStereoDelayVal)
	SXGP_STE_S,    // the left channel's delay in samples, 0: off
	SXGP_PARAMS
};
extern qboolean sx_r3d_gp;
void SX_R3D_GpParams( int *params, qboolean active, int gain );

#endif // S_DSP_GP_H
