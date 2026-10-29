#pragma once
#include <stddef.h>
#include "brook_system_message.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Returns 0 on a malformed record or truncation; no partial message is usable. */
int brook_system_message_format(const BrookSystemMessage *, char *simple, size_t simple_size,
                                char *technical, size_t technical_size);
#ifdef __cplusplus
}
#endif
