#pragma once
#include <stdint.h>

/* Fixed, bounded record shared by the recovery worker and user-mode presenter.
 * Flags describe measured facts; absence means unknown, never implicitly zero. */
enum {
    BROOK_SYSTEM_MESSAGE_VERSION = 1,
    BROOK_SYS_SYSTEM_MESSAGE = 0xB41,
    BROOK_MESSAGE_APP_CLOSED_MEMORY = 1,
    BROOK_MESSAGE_LAUNCH_MEMORY = 2,
    BROOK_MESSAGE_RECOVERY_STALLED = 3,
    BROOK_MESSAGE_NO_MEMORY_VICTIM = 4,
    BROOK_MESSAGE_TERMINATION_REQUESTED = 5,
    BROOK_MESSAGE_PHYSICAL_VALID = 1u << 0,
    BROOK_MESSAGE_AFTER_VALID = 1u << 1,
    BROOK_MESSAGE_RELEASED_VALID = 1u << 2,
    BROOK_MESSAGE_PAGING_CONFIGURED = 1u << 3,
    BROOK_MESSAGE_PAGING_USAGE_VALID = 1u << 4,
    BROOK_MESSAGE_PAGING_FULL = 1u << 5,
    BROOK_MESSAGE_PAGING_EXPERIMENTAL = 1u << 6,
    BROOK_MESSAGE_PAGING_STATE_VALID = 1u << 7
};
typedef struct BrookSystemMessage {
    uint32_t version, size, code, flags;
    uint64_t sequence, monotonic_ms, victim_incarnation;
    uint32_t victim_pid, victim_uid;
    uint64_t requested_bytes, physical_total_bytes, physical_free_before_bytes;
    uint64_t physical_free_after_bytes, released_bytes;
    uint64_t paging_capacity_bytes, paging_used_bytes;
    char app_name[64];
    char executable[256];
} BrookSystemMessage;

#ifdef __cplusplus
static_assert(sizeof(BrookSystemMessage)==424,"system message ABI layout");
#else
_Static_assert(sizeof(BrookSystemMessage)==424,"system message ABI layout");
#endif
