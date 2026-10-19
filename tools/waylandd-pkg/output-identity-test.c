/* Exercise real output events, including compatibility with older clients. */
#define _GNU_SOURCE
#include <assert.h>
static long test_syscall(long number, ...) { (void)number; return 0; }
#define syscall test_syscall
#define main brook_waylandd_main
#include "waylandd.c"
#undef main
#undef syscall

int main(void) {
    for (unsigned version = 1; version <= 4; ++version) {
        int sockets[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        struct wl_display *display = wl_display_create(); assert(display);
        struct wl_client *client = wl_client_create(display, sockets[0]); assert(client);
        assert(!g_outputs);
        output_bind(client, NULL, version, 0);
        assert(g_outputs && !g_outputs->next);
        const uint32_t object = wl_resource_get_id(g_outputs->resource);
        wl_display_flush_clients(display);
        uint32_t wire[512];
        ssize_t bytes = recv(sockets[1], wire, sizeof(wire), MSG_DONTWAIT);
        assert(bytes > 0);
        unsigned counts[6] = {0}, last = 99;
        for (size_t offset = 0; offset < (size_t)bytes;) {
            uint32_t *event = (void *)((char *)wire + offset);
            uint32_t size = event[1] >> 16, opcode = event[1] & 65535;
            assert(size >= 8 && !(size & 3) && offset + size <= (size_t)bytes);
            assert(event[0] == object && opcode < 6);
            ++counts[opcode]; last = opcode;
            if (opcode == WL_OUTPUT_NAME || opcode == WL_OUTPUT_DESCRIPTION) {
                assert(size >= 12 && event[2] && event[2] <= size - 12);
                const char *value = (char *)&event[3];
                assert(value[event[2] - 1] == 0);
                assert(!strcmp(value, opcode == WL_OUTPUT_NAME ? "BROOK-1" : "Brook virtual display"));
            }
            offset += size;
        }
        assert(counts[WL_OUTPUT_GEOMETRY] == 1 && counts[WL_OUTPUT_MODE] == 1);
        assert(counts[WL_OUTPUT_SCALE] == (version >= 2));
        assert(counts[WL_OUTPUT_DONE] == (version >= 2));
        assert(counts[WL_OUTPUT_NAME] == (version >= 4));
        assert(counts[WL_OUTPUT_DESCRIPTION] == (version >= 4));
        if (version >= 2) assert(last == WL_OUTPUT_DONE);
        wl_client_destroy(client); assert(!g_outputs);
        close(sockets[1]); wl_display_destroy(display);
    }
    puts("OUTPUT_IDENTITY_PASS v1/v2/v3/v4 event versions, names and done ordering");
    return 0;
}
