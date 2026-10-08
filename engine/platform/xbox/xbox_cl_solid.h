/*
xbox_cl_solid.h - skip a CL_SetSolidEntities that would rebuild the same lists
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
Resonance3D (docs/r3d/divergences.md #156). CL_SetSolidEntities
(engine/client/dll_int/cl_pmove.c, :467 in the reference) rebuilds the
client's three physent lists - physents, visents, moveents - from the
last parsed frame three times a frame in single player: at the end of
CL_ParsePacketEntities (cl_frame.c:902), at the end of CL_ReadPackets
(cl_main.c:3044) and at the start of CL_CreateCmd (cl_main.c:781); the
GoldSrc and Quake protocol parsers call it too (cl_parse_gs.c:401,
cl_qparse.c:1084). Only the first follows a new frame.

What a rebuild writes is a function of: the frame cl.frames[parsecountmod]
(its validity, its slots in cls.packet_entities and their states), the
model each state's modelindex names (cl.models[] and the model_t behind
it), cl.worldmodel, cl.playernum, cl.maxclients - and one thing it reads
from the lists themselves: CL_CopyEntityToPhysEnt chooses between
pe->model and pe->studiomodel for a non-SOLID_BBOX entity from pe->solid
(:341) before it writes pe->solid = state->solid (:357), so a slot's
first rebuild after it held another entity can choose differently from
the next one. Every other field it writes comes from the state, the
model or cl.maxclients (the name's bytes after its NUL are left as they
were, the same by every rebuild). So:

  - a rebuild with the same inputs as the last one changes only that
    choice, slot by slot, to what pe->solid (now the state's solid) says;
    Xbox_CL_SolidReuse makes exactly that change instead of rebuilding
    (the "settle" pass: a few loads per slot instead of a copy of ~60
    fields), and
  - the rebuild after that changes nothing, so it is skipped.

The inputs are a key, compared with the one the last rebuild saw:

  - the frame: cl.parsecount and the frame's validity, first slot and
    entity count. A frame's states are written only while it is parsed
    (CL_DeltaEntity, and CL_ProcessPacket's messagenum, msg_time and the
    local player's override, before the rebuild at the end of
    CL_ParsePacketEntities); every parse takes new slots
    (cls.next_client_entities only grows), so a new parse changes the key;
  - the models: cl.models[] is written only by CL_PrecacheResources, which
    calls Xbox_CL_SolidInvalidate; a model_t is loaded or freed through
    Mod_LoadModel and Mod_FreeModel, which bump xbox_model_generation
    (engine/platform/xbox/xbox_pm_cull.c), in the key. Two other writers
    do not, and cannot change a list: pfnLoadMapSprite reloads a sprite
    into its slot (CL_LoadHudSprite), and no sprite is a physent or a
    visent while a moveent reads only its name and type, which a reload
    of the same file keeps; Mod_FindName renames only a free slot, which
    cl.models[] does not point to;
  - cl.worldmodel, cl.playernum, cl.maxclients, clgame.pmove and the
    packet entity ring itself are in the key;
  - the lists: besides the rebuild, the engine writes physent slots only
    in CL_SetSolidPlayers, which appends at numphysent - above the built
    slots unless numphysent was lowered first, and then
    Xbox_CL_SolidPlayers drops the key; the counts are changed by
    CL_SetSolidPlayers, CL_PopPMStates (back to what CL_PushPMStates
    saved) and PM_ClearPhysEnts (CL_ClearState), and the lists are reused
    only when all three are what the rebuild left. The client DLL gets
    the lists through EV_GetPhysent/GetVisent (pfnGetPhysent and
    pfnGetVisent in cl_game.c), which hand out writable slots: each call
    drops the key, so a DLL that writes a slot gets upstream's rebuild on
    the next call (the HL client calls them only in weapon events and the
    trace-hit effects). It also gets them through the playermove_t handed
    to its prediction (HUD_PlayerMove): CL_RunUsercmd drops the key right
    after that call too, so no client DLL write survives into a skipped
    rebuild, whatever the DLL does (Codex review of #156). This costs no
    skip in single player: CL_CreateCmd's rebuild and its prediction run
    in CL_SendCommand, before the server frame, and the next
    CL_ParsePacketEntities rebuild sets a new key after that prediction.
*/

#ifndef XBOX_CL_SOLID_H
#define XBOX_CL_SOLID_H

/* Non-zero: the lists are now what CL_SetSolidEntities would rebuild
   (after settling them, the first time on a key), so it can return. */
int Xbox_CL_SolidReuse( void );

/* CL_SetSolidEntities rebuilt the lists: remember their inputs and counts. */
void Xbox_CL_SolidBuilt( void );

/* Something the key does not see changed (cl.models[]). */
void Xbox_CL_SolidInvalidate( void );

/* CL_SetSolidPlayers is about to append at numphysent: below the built
   slots it would overwrite them. */
void Xbox_CL_SolidPlayers( void );

#endif /* XBOX_CL_SOLID_H */
