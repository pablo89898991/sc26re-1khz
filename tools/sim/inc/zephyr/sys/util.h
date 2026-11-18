#pragma once
#define BIT(n) (1UL << (n))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ARG_UNUSED(x) (void)(x)
#define BUILD_ASSERT(c, ...) _Static_assert(c, "" __VA_ARGS__)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IS_ENABLED(x) 1
#ifndef __packed
#define __packed __attribute__((packed))
#endif
