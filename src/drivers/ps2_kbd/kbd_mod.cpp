// kbd_mod.cpp — PS/2 keyboard driver module for Brook OS.
//
// Calling KbdInit() directly from the kernel is optional; this module
// exists so keyboard support can be loaded at runtime rather than baked
// unconditionally into the kernel binary.
//
// The module is stored in /boot/drivers/ps2_kbd.mod and loaded after
// /boot is mounted.  Because module loading happens before 'sti', the
// IRQ handler is installed before interrupts are re-enabled.

#include "module_abi.h"
#include "keyboard.h"
#include "serial.h"
#include "kprintf.h"

MODULE_IMPORT_SYMBOL(KbdInit);
MODULE_IMPORT_SYMBOL(KbdIsAvailable);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(SerialPuts);

using namespace brook;

static int KbdModuleInit()
{
    SerialPuts("ps2_kbd module: init\n");

    if (KbdIsAvailable())
    {
        // Already initialised (e.g. kernel called KbdInit() itself).
        KPrintf("ps2_kbd module: keyboard already initialized\n");
        return 0;
    }

    KbdInit();
    KPrintf("ps2_kbd module: PS/2 keyboard initialized\n");
    return 0;
}

static void KbdModuleExit()
{
    SerialPuts("ps2_kbd module: exit (IRQ not unmasked — keyboard stays active)\n");
}

DECLARE_MODULE("ps2_kbd", KbdModuleInit, KbdModuleExit,
               "PS/2 keyboard driver (IRQ1, scan code set 1)");
