#pragma once
// Minimal stddef.h for freestanding kernel use.

#ifndef __SIZE_TYPE__
typedef unsigned long size_t;
#else
typedef __SIZE_TYPE__ size_t;
#endif

#ifndef __PTRDIFF_TYPE__
typedef long ptrdiff_t;
#else
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)
