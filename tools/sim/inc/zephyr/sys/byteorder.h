#pragma once
#include <stdint.h>
static inline void sys_put_le16(uint16_t v, uint8_t *d) { d[0] = v; d[1] = v >> 8; }
static inline void sys_put_le32(uint32_t v, uint8_t *d) { sys_put_le16(v, d); sys_put_le16(v >> 16, d + 2); }
static inline uint16_t sys_get_le16(const uint8_t *s) { return s[0] | (s[1] << 8); }
static inline uint32_t sys_get_le32(const uint8_t *s) { return sys_get_le16(s) | ((uint32_t)sys_get_le16(s + 2) << 16); }
