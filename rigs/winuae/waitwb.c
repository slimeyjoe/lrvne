/* SPDX-License-Identifier: BSD-2-Clause */
/* Waits until the Workbench screen is open in a given display mode: the event the rig waits
 * for after IPrefs and LoadWB put the Workbench in the mode of its prefs (run-lib.sh
 * emu_wbmode), instead of a number of seconds (a guest's seconds depend on the host when the
 * CPU runs at cpu_speed=max). No deadline: the rig's timeout is the backstop; Ctrl-C ends it.
 *
 *   waitwb [MODEID]     MODEID in hex (0x29004); without it, any Workbench screen
 *
 * Prints the mode it found. */
#include <stdio.h>
#include <stdlib.h>
#include <exec/types.h>
#include <graphics/modeid.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

int
main(int argc, char **argv)
{
	ULONG want = INVALID_ID, mode = INVALID_ID;
	struct Screen *s;
	int found = 0;

	if (argc > 1) {
		/* hex, with or without 0x (vbcc's strtoul does not skip it in base 16) */
		const char *p = argv[1];
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
			p += 2;
		want = strtoul(p, 0, 16);
		printf("waitwb: waiting for the Workbench in mode %08lx\n", (unsigned long)want);
	}

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
	GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
	if (!IntuitionBase || !GfxBase)
		return 20;
	while (!found) {
		if ((s = LockPubScreen((STRPTR)"Workbench")) != NULL) {
			mode = GetVPModeID(&s->ViewPort);
			found = want == INVALID_ID || mode == want;
			UnlockPubScreen(NULL, s);
		}
		if (found || (SetSignal(0, 0) & SIGBREAKF_CTRL_C))
			break;
		Delay(1);
	}
	if (found)
		printf("waitwb: the Workbench is open in mode %08lx\n", (unsigned long)mode);
	else
		printf("waitwb: Ctrl-C (the Workbench was in mode %08lx)\n", (unsigned long)mode);
	CloseLibrary((struct Library *)GfxBase);
	CloseLibrary((struct Library *)IntuitionBase);
	return found ? 0 : 5;
}
