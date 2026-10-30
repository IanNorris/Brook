#pragma once
#include <stddef.h>
#include <stdint.h>
/* A read-only view of trusted-path metadata in a bounded RAM initrd. No file
 * descriptors, host devices or kernel mounts. Single-threaded invocation.
 * MISSING is a lookup result, not a global filesystem-integrity certificate,
 * and must never authorize replacing a potentially referenced application slot. */
enum BrookInitrdRead {BROOK_INITRD_OK=0,BROOK_INITRD_MISSING=1,BROOK_INITRD_INVALID=2,BROOK_INITRD_TOO_LARGE=3};
/* Exact bytes, never silently truncated. out_bytes excludes any terminator.
 * The destination has no NUL terminator added: callers validate text length. */
int BrookInitrdMetadata(const uint8_t* image,size_t image_bytes,const char* path,
                        uint8_t* output,size_t capacity,size_t* out_bytes);
