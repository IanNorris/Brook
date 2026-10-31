#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Read-only validation against the exact boot-time image admission rules. */
int hardware_store_validate(int fd, const char *record, size_t record_bytes);
#ifdef __cplusplus
}
#endif
