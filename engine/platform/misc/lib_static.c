/*
lib_static.c - static linking support
Copyright (C) 2018 Flying With Gauss

This program is free software: you can redistribute it and/sor modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "platform/platform.h"
#include "library.h"

#if XASH_LIB == LIB_STATIC
typedef struct table_s
{
	const char *name;
	void *pointer;
} table_t;


#include "generated_library_tables.h"

static void *Lib_Find(table_t *tbl, const char *name )
{
	while( tbl->name )
	{
		if( !Q_strcmp( tbl->name, name) )
			return tbl->pointer;
		tbl++;
	}

	return 0;
}

void *COM_LoadLibrary( const char *dllname, int build_ordinals_table, qboolean directpath )
{
	return Lib_Find((table_t*)libs, dllname);
}

void COM_FreeLibrary( void *hInstance )
{
	// impossible
}

void *COM_GetProcAddress( void *hInstance, const char *name )
{
	return Lib_Find( hInstance, name );
}

void *COM_FunctionFromName( void *hInstance, const char *pName )
{
	return Lib_Find( hInstance, pName );
}


const char *COM_NameForFunction( void *hInstance, void *function )
{
#if XASH_XBOX
	// XS.2 (docs/r3d/fork-plan.md): a saved callback is written by name, never as an offset from GameDLLInit, which is
	// only true for the link that wrote it. The server's export table carries one `Think@CFoo` row per function game
	// code can store in a saved pointer (tools/xbox-gen-callback-table.py); the image links without folding
	// so each has its own address. A name has an '@'; entity classnames and the API exports do not.
	table_t *tbl = (table_t*)hInstance;

	if( tbl && function )
	{
		for( ; tbl->name; tbl++ )
		{
			if( tbl->pointer == function && Q_strchr( tbl->name, '@' ))
				return tbl->name;
		}
	}

	// Never an offset: a pointer with no name would load wrong on any other link. The SDK's save then refuses the entity.
	Con_Printf( S_ERROR "%s: no saved name for function %p\n", __func__, function );
	return NULL;
#elif defined( XASH_ALLOW_SAVERESTORE_OFFSETS )
	return COM_OffsetNameForFunction( function );
#else
	return NULL;
#endif
}
#endif // XASH_LIB == LIB_STATIC
