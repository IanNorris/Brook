/* Host behavioral test for the native port's input adapter, not a kernel test.
 * cc -std=gnu99 -I tools/quake2/source tools/quake2/source/in_brook_test.c -o /tmp/q2-input-test
 */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
static long test_syscall(long number, ...);
#define syscall test_syscall
#include "in_brook.c"
#undef syscall
client_state_t cl;
client_static_t cls;
unsigned int brook_q2_window = 7;
static struct brook_q2_event pending[32];
static unsigned count, granted = 41, live, downs[256], ups[256], clears, quits;
int Sys_Milliseconds(void) { return 100; }
void Com_Printf(char *format, ...) { (void)format; }
void Cbuf_AddText(char *text) { assert(!strcmp(text, "quit\n")); ++quits; }
void Key_Event(int key, qboolean down, unsigned time) {
    (void)time; assert(key > 0 && key < 256);
    if (down) ++downs[key]; else ++ups[key];
}
void Key_ClearStates(void) { ++clears; }
cvar_t *Cvar_Get(char *name, char *value, int flags) {
    static cvar_t sensitivity; (void)name; (void)value; (void)flags;
    sensitivity.value = 3; return &sensitivity;
}
static long test_syscall(long number, ...) {
    va_list ap; va_start(ap, number); assert(va_arg(ap, long) == 7);
    long result = 0;
    if (number == 510) {
        struct brook_q2_event *out = (void *)va_arg(ap, long);
        assert(va_arg(ap, long) == 32); memcpy(out, pending, count * sizeof(*out));
        result = count; count = 0;
    } else {
        assert(number == 0xB0A);
        long op = va_arg(ap, long); unsigned token = va_arg(ap, long);
        if (op == 0) result = token == live ? live : 0;
        else if (op == 1) result = live = ++granted;
        else { assert(op == 2 && token == live); live = 0; }
    }
    va_end(ap); return result;
}
static void event(unsigned type, unsigned scan, int x, int y, unsigned token) {
    assert(count < 32); pending[count++] = (struct brook_q2_event){type,scan,0,0,x,y,token};
}
int main(void) {
    usercmd_t command = {0}; cls.key_dest = key_game; IN_Init();
    event(3,0,0,0,0); Brook_SendKeyEvents(); assert(live && downs[K_MOUSE1] == 0);
    unsigned original = live;
    event(6,0,100,20,live); event(3,0,0,0,live); event(4,0,0,0,live); Brook_SendKeyEvents(); IN_Move(&command);
    assert(cl.viewangles[YAW] < -6.5f && cl.viewangles[PITCH] > 1.3f);
    assert(ups[K_MOUSE1] == 1); /* full press/release, no stuck firing */
    float yaw = cl.viewangles[YAW];
    event(6,0,999,0,live-1); Brook_SendKeyEvents(); IN_Move(&command);
    assert(cl.viewangles[YAW] == yaw); /* stale capture cannot move camera */
    event(0,0x80,0,0,0); event(1,0x80,0,0,0); Brook_SendKeyEvents();
    assert(downs[K_UPARROW] == 1 && ups[K_UPARROW] == 1);
    event(3,1,0,0,live); event(0x82,0,0,0,0); Brook_SendKeyEvents();
    assert(!live && !capture_token && ups[K_MOUSE2] == 1 && clears == 1);
    event(3,0,0,0,0); Brook_SendKeyEvents(); assert(live > original);
    live = 0; Brook_SendKeyEvents(); assert(!capture_token); /* kernel revoked lease */
    IN_Frame(); assert(!live); /* no immediate re-grab after Ctrl+Alt+Esc */
    cls.key_dest = key_menu; event(3,0,0,0,0); Brook_SendKeyEvents(); assert(!live);
    cls.key_dest = key_game; event(3,0,0,0,0); Brook_SendKeyEvents(); assert(live);
    cls.key_dest = key_console; IN_Frame(); assert(!live && !buttons);
    event(0x80,0,0,0,0); Brook_SendKeyEvents(); assert(quits == 1);
    IN_Shutdown(); puts("Q2_INPUT_PASS raw-motion buttons stale-token focus menu release extended-keys close");
    return 0;
}
