//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Handles WAD file header, directory, lump I/O.
//




#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"

#include "config.h"
#include "d_iwad.h"
#include "i_swap.h"
#include "i_system.h"
#include "i_video.h"
#include "m_misc.h"
#include "z_zone.h"

#include "w_wad.h"

typedef struct
{
    // Should be "IWAD" or "PWAD".
    char		identification[4];		
    int			numlumps;
    int			infotableofs;
} PACKEDATTR wadinfo_t;


typedef struct
{
    int			filepos;
    int			size;
    char		name[8];
} PACKEDATTR filelump_t;

//
// GLOBALS
//

// Location of each lump on disk.

lumpinfo_t *lumpinfo;		
unsigned int numlumps = 0;

// Hash table for fast lookups

static lumpinfo_t **lumphash;

// Hash function used for lump names.

unsigned int W_LumpNameHash(const char *s)
{
    // This is the djb2 string hash function, modded to work on strings
    // that have a maximum length of 8.

    unsigned int result = 5381;
    unsigned int i;

    for (i=0; i < 8 && s[i] != '\0'; ++i)
    {
        result = ((result << 5) ^ result ) ^ toupper((int)s[i]);
    }

    return result;
}

// Increase the size of the lumpinfo[] array to the specified size.
static void ExtendLumpInfo(int newnumlumps)
{
    lumpinfo_t *newlumpinfo;
    unsigned int i;

    newlumpinfo = calloc(newnumlumps, sizeof(lumpinfo_t));

    if (newlumpinfo == NULL)
    {
	I_Error ("Couldn't realloc lumpinfo");
    }

    // Copy over lumpinfo_t structures from the old array. If any of
    // these lumps have been cached, we need to update the user
    // pointers to the new location.
    for (i = 0; i < numlumps && i < newnumlumps; ++i)
    {
        memcpy(&newlumpinfo[i], &lumpinfo[i], sizeof(lumpinfo_t));

        if (newlumpinfo[i].cache != NULL)
        {
            Z_ChangeUser(newlumpinfo[i].cache, &newlumpinfo[i].cache);
        }

        // We shouldn't be generating a hash table until after all WADs have
        // been loaded, but just in case...
        if (lumpinfo[i].next != NULL)
        {
            int nextlumpnum = lumpinfo[i].next - lumpinfo;
            newlumpinfo[i].next = &newlumpinfo[nextlumpnum];
        }
    }

    // All done.
    free(lumpinfo);
    lumpinfo = newlumpinfo;
    numlumps = newnumlumps;
}

//
// LUMP BASED ROUTINES.
//

//
// W_AddFile
// All files are optional, but at least one file must be
//  found (PWAD, if all required lumps are present).
// Files with a .wad extension are wadlink files
//  with multiple lumps.
// Other files are single lumps with the base filename
//  for the lump name.

wad_file_t *W_AddFile (char *filename)
{
    wadinfo_t header;
    lumpinfo_t *lump_p;
    unsigned int i;
    wad_file_t *wad_file;
    int length;
    int startlump;
    filelump_t *fileinfo;
    filelump_t *filerover;
    int newnumlumps;

    // open the file and add to directory

    wad_file = W_OpenFile(filename);

    if (wad_file == NULL)
    {
		printf (" couldn't open %s\n", filename);
		return NULL;
    }

    newnumlumps = numlumps;

    if (strcasecmp(filename+strlen(filename)-3 , "wad" ) )
    {
    	// single lump file

        // fraggle: Swap the filepos and size here.  The WAD directory
        // parsing code expects a little-endian directory, so will swap
        // them back.  Effectively we're constructing a "fake WAD directory"
        // here, as it would appear on disk.

		fileinfo = Z_Malloc(sizeof(filelump_t), PU_STATIC, 0);
		fileinfo->filepos = LONG(0);
		fileinfo->size = LONG(wad_file->length);

        // Name the lump after the base of the filename (without the
        // extension).

		M_ExtractFileBase (filename, fileinfo->name);
		newnumlumps++;
    }
    else 
    {
    	// WAD file
        W_Read(wad_file, 0, &header, sizeof(header));

		if (strncmp(header.identification,"IWAD",4))
		{
			// Homebrew levels?
			if (strncmp(header.identification,"PWAD",4))
			{
			I_Error ("Wad file %s doesn't have IWAD "
				 "or PWAD id\n", filename);
			}

			// ???modifiedgame = true;
		}

		header.numlumps = LONG(header.numlumps);
		header.infotableofs = LONG(header.infotableofs);
		length = header.numlumps*sizeof(filelump_t);
		fileinfo = Z_Malloc(length, PU_STATIC, 0);

        W_Read(wad_file, header.infotableofs, fileinfo, length);
        newnumlumps += header.numlumps;
    }

    // Increase size of numlumps array to accomodate the new file.
    startlump = numlumps;
    ExtendLumpInfo(newnumlumps);

    lump_p = &lumpinfo[startlump];

    filerover = fileinfo;

    for (i=startlump; i<numlumps; ++i)
    {
		lump_p->wad_file = wad_file;
		lump_p->position = LONG(filerover->filepos);
		lump_p->size = LONG(filerover->size);
			lump_p->cache = NULL;
		strncpy(lump_p->name, filerover->name, 8);

			++lump_p;
			++filerover;
    }

    Z_Free(fileinfo);

    if (lumphash != NULL)
    {
        Z_Free(lumphash);
        lumphash = NULL;
    }

    return wad_file;
}



//
// W_NumLumps
//
int W_NumLumps (void)
{
    return numlumps;
}



//
// W_CheckNumForName
// Returns -1 if name not found.
//

int W_CheckNumForName (char* name)
{
    lumpinfo_t *lump_p;
    int i;

    // Do we have a hash table yet?

    if (lumphash != NULL)
    {
        int hash;
        
        // We do! Excellent.

        hash = W_LumpNameHash(name) % numlumps;

        lump_p = lumphash[hash];
        // Check for corrupted hash entry
        if (lump_p != NULL)
        {
            uintptr_t val = (uintptr_t)lump_p;
            uint32_t lo = (uint32_t)(val);
            uint32_t hi = (uint32_t)(val >> 32);
            if (hi != 0 && lo == hi)
            {
                fprintf(stderr, "CORRUPT: lumphash[%d]=0x%016lx name='%.8s' "
                        "lumphash=%p lumpinfo=%p\n",
                        hash, (unsigned long)val, name,
                        (void*)lumphash, (void*)lumpinfo);
                // Fall through to linear search instead of crashing
                goto linear_search;
            }
        }
        
        for (; lump_p != NULL; lump_p = lump_p->next)
        {
            if (!strncasecmp(lump_p->name, name, 8))
            {
                return lump_p - lumpinfo;
            }
        }
    } 
    else
    {
linear_search:
        // We don't have a hash table generate yet. Linear search :-(
        // 
        // scan backwards so patch lump files take precedence

        for (i=numlumps-1; i >= 0; --i)
        {
            if (!strncasecmp(lumpinfo[i].name, name, 8))
            {
                return i;
            }
        }
    }

    // TFB. Not found.

    return -1;
}




//
// W_GetNumForName
// Calls W_CheckNumForName, but bombs out if not found.
//
int W_GetNumForName (char* name)
{
    int	i;

    i = W_CheckNumForName (name);

    if (i < 0)
    {
        I_Error ("W_GetNumForName: %s not found!", name);
    }
 
    return i;
}


//
// W_LumpLength
// Returns the buffer size needed to load the given lump.
//
int W_LumpLength (unsigned int lump)
{
    if (lump >= numlumps)
    {
	I_Error ("W_LumpLength: %i >= numlumps", lump);
    }

    return lumpinfo[lump].size;
}



//
// W_ReadLump
// Loads the lump into the given buffer,
//  which must be >= W_LumpLength().
//
void W_ReadLump(unsigned int lump, void *dest)
{
    int c;
    lumpinfo_t *l;
	
    if (lump >= numlumps)
    {
	I_Error ("W_ReadLump: %i >= numlumps", lump);
    }

    l = lumpinfo+lump;
	
    I_BeginRead ();
	
    c = W_Read(l->wad_file, l->position, dest, l->size);

    if (c < l->size)
    {
	I_Error ("W_ReadLump: only read %i of %i on lump %i",
		 c, l->size, lump);	
    }

    I_EndRead ();
}




//
// W_CacheLumpNum
//
// Load a lump into memory and return a pointer to a buffer containing
// the lump data.
//
// 'tag' is the type of zone memory buffer to allocate for the lump
// (usually PU_STATIC or PU_CACHE).  If the lump is loaded as 
// PU_STATIC, it should be released back using W_ReleaseLumpNum
// when no longer needed (do not use Z_ChangeTag).
//

void *W_CacheLumpNum(int lumpnum, int tag)
{
    byte *result;
    lumpinfo_t *lump;

    if ((unsigned)lumpnum >= numlumps)
    {
	I_Error ("W_CacheLumpNum: %i >= numlumps", lumpnum);
    }

    lump = &lumpinfo[lumpnum];

    // Get the pointer to return.  If the lump is in a memory-mapped
    // file, we can just return a pointer to within the memory-mapped
    // region.  If the lump is in an ordinary file, we may already
    // have it cached; otherwise, load it into memory.

    if (lump->wad_file->mapped != NULL)
    {
        // Memory mapped file, return from the mmapped region.

        result = lump->wad_file->mapped + lump->position;
    }
    else if (lump->cache != NULL)
    {
        // Already cached, so just switch the zone tag.

        result = lump->cache;
        Z_ChangeTag(lump->cache, tag);
    }
    else
    {
        // Not yet loaded, so load it now

        lump->cache = Z_Malloc(W_LumpLength(lumpnum), tag, &lump->cache);
	W_ReadLump (lumpnum, lump->cache);
        result = lump->cache;
    }
	
    return result;
}



//
// W_CacheLumpName
//
void *W_CacheLumpName(char *name, int tag)
{
    return W_CacheLumpNum(W_GetNumForName(name), tag);
}

// 
// Release a lump back to the cache, so that it can be reused later 
// without having to read from disk again, or alternatively, discarded
// if we run out of memory.
//
// Back in Vanilla Doom, this was just done using Z_ChangeTag 
// directly, but now that we have WAD mmap, things are a bit more
// complicated ...
//

void W_ReleaseLumpNum(int lumpnum)
{
    lumpinfo_t *lump;

    if ((unsigned)lumpnum >= numlumps)
    {
	I_Error ("W_ReleaseLumpNum: %i >= numlumps", lumpnum);
    }

    lump = &lumpinfo[lumpnum];

    if (lump->wad_file->mapped != NULL)
    {
        // Memory-mapped file, so nothing needs to be done here.
    }
    else
    {
        Z_ChangeTag(lump->cache, PU_CACHE);
    }
}

void W_ReleaseLumpName(char *name)
{
    W_ReleaseLumpNum(W_GetNumForName(name));
}

#if 0

//
// W_Profile
//
int		info[2500][10];
int		profilecount;

void W_Profile (void)
{
    int		i;
    memblock_t*	block;
    void*	ptr;
    char	ch;
    FILE*	f;
    int		j;
    char	name[9];
	
	
    for (i=0 ; i<numlumps ; i++)
    {	
	ptr = lumpinfo[i].cache;
	if (!ptr)
	{
	    ch = ' ';
	    continue;
	}
	else
	{
	    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));
	    if (block->tag < PU_PURGELEVEL)
		ch = 'S';
	    else
		ch = 'P';
	}
	info[i][profilecount] = ch;
    }
    profilecount++;
#if ORIGCODE
    f = fopen ("waddump.txt","w");
    name[8] = 0;

    for (i=0 ; i<numlumps ; i++)
    {
	memcpy (name,lumpinfo[i].name,8);

	for (j=0 ; j<8 ; j++)
	    if (!name[j])
		break;

	for ( ; j<8 ; j++)
	    name[j] = ' ';

	fprintf (f,"%s ",name);

	for (j=0 ; j<profilecount ; j++)
	    fprintf (f,"    %c",info[i][j]);

	fprintf (f,"\n");
    }
    fclose (f);
#endif
}


#endif

// Generate a hash table for fast lookups

void W_GenerateHashTable(void)
{
    unsigned int i;

    // Free the old hash table, if there is one

    if (lumphash != NULL)
    {
        Z_Free(lumphash);
    }

    // Generate hash table
    if (numlumps > 0)
    {
        lumphash = Z_Malloc(sizeof(lumpinfo_t *) * numlumps, PU_STATIC, NULL);
        fprintf(stderr, "HASH_DIAG: Z_Malloc returned %p (size=%lu)\n",
                (void*)lumphash, (unsigned long)(sizeof(lumpinfo_t*) * numlumps));
        memset(lumphash, 0, sizeof(lumpinfo_t *) * numlumps);

        // CHECKPOINT 1: verify memset actually zeroed the table
        {
            unsigned int nonzero = 0;
            for (i = 0; i < numlumps; ++i)
                if (lumphash[i] != NULL) nonzero++;
            fprintf(stderr, "HASH_DIAG: after memset: nonzero=%u/%u\n", nonzero, numlumps);
        }

        for (i=0; i<numlumps; ++i)
        {
            unsigned int hash;

            hash = W_LumpNameHash(lumpinfo[i].name) % numlumps;

            // Hook into the hash table

            lumpinfo[i].next = lumphash[hash];
            lumphash[hash] = &lumpinfo[i];
        }

        // CHECKPOINT 2: validate hash table immediately after fill loop
        {
            unsigned int bad = 0;
            for (i = 0; i < numlumps; ++i)
            {
                uintptr_t val = (uintptr_t)lumphash[i];
                uint32_t lo = (uint32_t)(val);
                uint32_t hi = (uint32_t)(val >> 32);
                if (val != 0 && lo == hi)
                {
                    if (bad < 5)
                        fprintf(stderr, "HASH_VERIFY[%u]: DUPED 0x%016lx\n", i, (unsigned long)val);
                    bad++;
                }
            }
            fprintf(stderr, "HASH_VERIFY: lumphash=%p lumpinfo=%p numlumps=%u duped=%u\n",
                    (void*)lumphash, (void*)lumpinfo, numlumps, bad);
            for (i = 0; i < 8 && i < numlumps; ++i)
            {
                uintptr_t val = (uintptr_t)lumphash[i];
                uint32_t lo = (uint32_t)(val);
                uint32_t hi = (uint32_t)(val >> 32);
                fprintf(stderr, "HASH[%u]: 0x%016lx (lo=0x%08x hi=0x%08x %s)\n",
                        i, (unsigned long)val, lo, hi,
                        (lo == hi && val != 0) ? "DUPED!" : "ok");
            }
            if (bad > 0) {
                fprintf(stderr, "HASH_VERIFY: CORRUPTION DETECTED AT GENERATION TIME!\n");
                fprintf(stderr, "  sizeof(lumpinfo_t*)=%lu sizeof(lumpinfo_t)=%lu\n",
                        (unsigned long)sizeof(lumpinfo_t*),
                        (unsigned long)sizeof(lumpinfo_t));
                // Print what a valid pointer looks like
                fprintf(stderr, "  &lumpinfo[0]=%p &lumpinfo[1]=%p\n",
                        (void*)&lumpinfo[0], (void*)&lumpinfo[1]);
            }
        }
    }

    // All done!
}

int W_VerifyHashTable(const char *checkpoint)
{
    if (!lumphash || numlumps == 0) return 0;
    unsigned int bad = 0;
    for (unsigned int i = 0; i < numlumps; ++i)
    {
        uintptr_t val = (uintptr_t)lumphash[i];
        uint32_t lo = (uint32_t)(val);
        uint32_t hi = (uint32_t)(val >> 32);
        if (val != 0 && lo == hi) bad++;
    }
    if (bad > 0)
    {
        fprintf(stderr, "*** HASH CORRUPT at [%s]: %u/%u entries bad ***\n",
                checkpoint, bad, numlumps);
        for (unsigned int i = 0; i < 4 && i < numlumps; ++i)
        {
            uintptr_t val = (uintptr_t)lumphash[i];
            fprintf(stderr, "  [%u] = 0x%016lx\n", i, (unsigned long)val);
        }
    }
    else
    {
        fprintf(stderr, "HASH OK at [%s]\n", checkpoint);
    }
    return (int)bad;
}

// Lump names that are unique to particular game types. This lets us check
// the user is not trying to play with the wrong executable, eg.
// chocolate-doom -iwad hexen.wad.
static const struct
{
    GameMission_t mission;
    char *lumpname;
} unique_lumps[] = {
    { doom,    "POSSA1" },
    { heretic, "IMPXA1" },
    { hexen,   "ETTNA1" },
    { strife,  "AGRDA1" },
};

void W_CheckCorrectIWAD(GameMission_t mission)
{
    int i;
    int lumpnum;

    for (i = 0; i < arrlen(unique_lumps); ++i)
    {
        if (mission != unique_lumps[i].mission)
        {
            lumpnum = W_CheckNumForName(unique_lumps[i].lumpname);

            if (lumpnum >= 0)
            {
                I_Error("\nYou are trying to use a %s IWAD file with "
                        "the %s%s binary.\nThis isn't going to work.\n"
                        "You probably want to use the %s%s binary.",
                        D_SuggestGameName(unique_lumps[i].mission,
                                          indetermined),
                        PROGRAM_PREFIX,
                        D_GameMissionString(mission),
                        PROGRAM_PREFIX,
                        D_GameMissionString(unique_lumps[i].mission));
            }
        }
    }
}

