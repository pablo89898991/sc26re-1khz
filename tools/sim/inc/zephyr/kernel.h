#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include "sys/util.h"
#ifndef __packed
#define __packed __attribute__((packed))
#endif
int64_t k_uptime_get(void);
uint32_t k_uptime_get_32(void);
struct k_spinlock { int x; };
typedef int k_spinlock_key_t;
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l) { (void)l; return 0; }
static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t k) { (void)l; (void)k; }
