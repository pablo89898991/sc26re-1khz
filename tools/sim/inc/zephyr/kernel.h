#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include "sys/util.h"
#ifndef USEC_PER_MSEC
#define USEC_PER_MSEC 1000U
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
int64_t k_uptime_get(void);
uint32_t k_uptime_get_32(void);
/* the simulator's clock counts whole ms: ticks are us here */
static inline int64_t k_uptime_ticks(void) { return k_uptime_get() * 1000; }
static inline uint64_t k_ticks_to_us_floor64(uint64_t t) { return t; }
struct k_spinlock { int x; };
typedef int k_spinlock_key_t;
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l) { (void)l; return 0; }
static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t k) { (void)l; (void)k; }

struct k_mutex { int x; };
#define K_MUTEX_DEFINE(name) struct k_mutex name
#define K_FOREVER 0
#define K_NO_WAIT 0
static inline int k_mutex_lock(struct k_mutex *m, int t) { (void)m; (void)t; return 0; }
static inline int k_mutex_unlock(struct k_mutex *m) { (void)m; return 0; }
struct k_work;
typedef void (*k_work_handler_t)(struct k_work *);
struct k_work { k_work_handler_t h; };
#define K_WORK_DEFINE(name, fn) struct k_work name = { fn }
static inline int k_work_submit(struct k_work *w) { w->h(w); return 0; }
static inline void k_work_init(struct k_work *w, k_work_handler_t h) { w->h = h; }
