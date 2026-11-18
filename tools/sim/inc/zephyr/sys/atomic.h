#pragma once
typedef long atomic_t;
typedef long atomic_val_t;
static inline long atomic_get(const atomic_t *a) { return *a; }
static inline long atomic_set(atomic_t *a, long v) { long o = *a; *a = v; return o; }
static inline long atomic_inc(atomic_t *a) { return (*a)++; }
static inline long atomic_dec(atomic_t *a) { return (*a)--; }
static inline int atomic_cas(atomic_t *a, long o, long n) { if(*a == o) { *a = n; return 1; } return 0; }
#define ATOMIC_INIT(v) (v)
static inline long atomic_add(atomic_t *a, long v) { long o = *a; *a += v; return o; }
