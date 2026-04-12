// ps2_mouse module — loadable wrapper for the PS/2 mouse driver.
//
// The actual ISR and mouse logic live in kernel/src/mouse.cpp (compiled
// with -mgeneral-regs-only). This module just calls MouseInit().

#include "module_abi.h"
#include "mouse.h"
#include "serial.h"
#include "kprintf.h"

MODULE_IMPORT_SYMBOL(MouseInit);
MODULE_IMPORT_SYMBOL(MouseIsAvailable);
MODULE_IMPORT_SYMBOL(KPrintf);
MODULE_IMPORT_SYMBOL(SerialPuts);

using namespace brook;

static int Ps2MouseModuleInit()
{
    if (MouseIsAvailable()) return 0; // Already initialized
    MouseInit();
    return 0;
}

static void Ps2MouseModuleExit()
{
    SerialPuts("ps2_mouse module: exit\n");
}

DECLARE_MODULE("ps2_mouse", Ps2MouseModuleInit, Ps2MouseModuleExit,
               "PS/2 mouse driver (IRQ12, 3-byte packets)");
