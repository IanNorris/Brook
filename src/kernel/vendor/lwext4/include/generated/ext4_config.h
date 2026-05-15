/* Brook OS configuration for lwext4 */

#ifndef EXT4_CONFIG_GENERATED_H_
#define EXT4_CONFIG_GENERATED_H_

/* ext4 feature level — full ext4 with extents and journaling */
#define CONFIG_EXT_FEATURE_SET_LVL  4  /* F_SET_EXT4 */

/* Enable journaling (crash recovery on unclean shutdown) */
#define CONFIG_JOURNALING_ENABLE    1

/* Enable extended attributes */
#define CONFIG_XATTR_ENABLE         1

/* Enable extents (better large-file performance) */
#define CONFIG_EXTENTS_ENABLE       1

/* Use our own errno definitions (no libc) */
#define CONFIG_HAVE_OWN_ERRNO       1

/* Minimal debug output — route through SerialPrintf */
#define CONFIG_DEBUG_PRINTF         1
#define CONFIG_DEBUG_ASSERT         1
#define CONFIG_HAVE_OWN_ASSERT      1

/* Block device cache — 1024 blocks (~4 MB for 4KB blocks) for good cache hit rate */
#define CONFIG_BLOCK_DEV_CACHE_SIZE 1024
#define CONFIG_BLOCK_DEV_ENABLE_STATS 0

/* Use Brook's memory allocator */
#define CONFIG_USE_USER_MALLOC      1

/* Forward-declare the user memory functions (defined in ext4_osal_brook.c).
 * lwext4's ext4_types.h maps ext4_malloc/calloc/realloc/free to these. */
#include <stddef.h>
void* ext4_user_malloc(size_t size);
void* ext4_user_calloc(size_t nmemb, size_t size);
void* ext4_user_realloc(void* ptr, size_t size);
void  ext4_user_free(void* ptr);

/* Not big-endian (x86-64) */
/* CONFIG_BIG_ENDIAN is NOT defined */

/* Don't use the default config — we provide our own */
#define CONFIG_USE_DEFAULT_CFG      0

#endif /* EXT4_CONFIG_GENERATED_H_ */
