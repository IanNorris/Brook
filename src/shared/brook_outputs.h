#ifndef BROOK_OUTPUTS_H
#define BROOK_OUTPUTS_H
/* Display outputs (multi-output D0). The desktop is the union of the
 * enabled outputs and starts at 0,0; WM_GET_SCREEN_INFO (520) keeps
 * returning its size. Output 0 is the primary output.
 *
 * syscall(BROOK_SYS_WM_GET_OUTPUTS, header, outputs, capacity):
 *   header   (required) receives the total output count and a generation
 *            that changes whenever any output's geometry, mode, scale or
 *            presence changes; poll it to notice a new layout.
 *   outputs  receives min(count, capacity) descriptors; may be 0 when
 *            capacity is 0 (a count/generation query).
 *   returns  the number of descriptors written, or -EFAULT / -EINVAL. */
#include <stdint.h>

#define BROOK_SYS_WM_GET_OUTPUTS 527
#define BROOK_OUTPUTS_MAX 4

enum {
    BROOK_OUTPUT_CONNECTED = 1u,
    BROOK_OUTPUT_ENABLED = 2u,
    BROOK_OUTPUT_PRIMARY = 4u,
};

struct brook_outputs_header {
    uint32_t count;
    uint32_t reserved;     /* 0 */
    uint64_t generation;
};

struct brook_output_desc {
    uint32_t id;           /* 0 = primary */
    int32_t x, y;          /* desktop position, physical pixels */
    uint32_t width, height;
    uint32_t refresh_mhz;  /* 60000 = 60 Hz; nominal on firmware framebuffers */
    uint32_t scale_percent;
    uint32_t flags;        /* BROOK_OUTPUT_* */
    char name[16];         /* "BROOK-1", NUL-terminated */
};

#ifdef __cplusplus
static_assert(sizeof(struct brook_outputs_header) == 16, "brook_outputs_header ABI");
static_assert(sizeof(struct brook_output_desc) == 48, "brook_output_desc ABI");
#else
_Static_assert(sizeof(struct brook_outputs_header) == 16, "brook_outputs_header ABI");
_Static_assert(sizeof(struct brook_output_desc) == 48, "brook_output_desc ABI");
#endif

#if !defined(__BROOK_KERNEL__) && !defined(BROOK_HOST_TEST)
#include <sys/syscall.h>
#include <unistd.h>
/* Userspace wrapper: descriptors written, or -1 with errno set. */
static inline long brook_wm_get_outputs(struct brook_outputs_header *header,
                                        struct brook_output_desc *outputs, uint32_t capacity)
{
    return syscall(BROOK_SYS_WM_GET_OUTPUTS, (long)header, (long)outputs, (long)capacity);
}
#endif
#endif
