#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"

uint32_t* DG_ScreenBuffer = 0;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

	DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
	fprintf(stderr, "DIAG: DG_ScreenBuffer=%p (end=%p)\n",
	        (void*)DG_ScreenBuffer,
	        (void*)(DG_ScreenBuffer + DOOMGENERIC_RESX * DOOMGENERIC_RESY));

	DG_Init();

	D_DoomMain ();
}

