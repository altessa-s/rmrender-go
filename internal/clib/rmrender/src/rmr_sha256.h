/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* SHA-256 (FIPS 180-4), self-contained and taken incrementally: the digest
 * the PDF writers key shared objects on. Internal; each user compiles its own
 * copy. With the Armv8 SHA-2 instructions available at compile time the
 * blocks go through them; otherwise through portable C. */
#ifndef RMR_SHA256_H
#define RMR_SHA256_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__ARM_FEATURE_SHA2) && defined(__aarch64__) && \
    !defined(RMR_SHA256_PORTABLE)
#include <arm_neon.h>
#define RMR_SHA256_ARM 1
#endif

typedef struct {
  uint8_t bytes[32];
} rmr_sha256_digest;

typedef struct {
  uint32_t state[8];
  uint64_t length; /* bytes taken */
  uint8_t block[64];
  size_t used; /* bytes waiting in block */
} rmr_sha256;

static const uint32_t rmr_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline void rmr_sha256_init(rmr_sha256* h) {
  static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                      0xa54ff53a, 0x510e527f, 0x9b05688c,
                                      0x1f83d9ab, 0x5be0cd19};
  memcpy(h->state, initial, sizeof initial);
  h->length = 0;
  h->used = 0;
}

static inline uint32_t rmr_sha256_rotate(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32 - n));
}

/* Runs count 64-byte blocks through the compression function. */
static inline void rmr_sha256_blocks(uint32_t state[8], const uint8_t* p,
                                     size_t count) {
#ifdef RMR_SHA256_ARM
  uint32x4_t abcd = vld1q_u32(state), efgh = vld1q_u32(state + 4);
  for (; count; count--, p += 64) {
    uint32x4_t m[4];
    for (unsigned i = 0; i < 4; i++)
      m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
    uint32x4_t a0 = abcd, e0 = efgh;
    for (unsigned i = 0; i < 16; i++) {
      uint32x4_t w = vaddq_u32(m[i % 4], vld1q_u32(rmr_sha256_k + 4 * i));
      if (i < 12)
        m[i % 4] = vsha256su1q_u32(vsha256su0q_u32(m[i % 4], m[(i + 1) % 4]),
                                   m[(i + 2) % 4], m[(i + 3) % 4]);
      uint32x4_t before = abcd;
      abcd = vsha256hq_u32(abcd, efgh, w);
      efgh = vsha256h2q_u32(efgh, before, w);
    }
    abcd = vaddq_u32(abcd, a0);
    efgh = vaddq_u32(efgh, e0);
  }
  vst1q_u32(state, abcd);
  vst1q_u32(state + 4, efgh);
#else
  for (; count; count--, p += 64) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
             (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (unsigned i = 16; i < 64; i++) {
      uint32_t s0 = rmr_sha256_rotate(w[i - 15], 7) ^
                    rmr_sha256_rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rmr_sha256_rotate(w[i - 2], 17) ^
                    rmr_sha256_rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3],
             e = state[4], f = state[5], g = state[6], k = state[7];
    for (unsigned i = 0; i < 64; i++) {
      uint32_t t1 = k +
                    (rmr_sha256_rotate(e, 6) ^ rmr_sha256_rotate(e, 11) ^
                     rmr_sha256_rotate(e, 25)) +
                    ((e & f) ^ (~e & g)) + rmr_sha256_k[i] + w[i];
      uint32_t t2 = (rmr_sha256_rotate(a, 2) ^ rmr_sha256_rotate(a, 13) ^
                     rmr_sha256_rotate(a, 22)) +
                    ((a & b) ^ (a & c) ^ (b & c));
      k = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += k;
  }
#endif
}

/* Takes n more bytes. */
static inline void rmr_sha256_update(rmr_sha256* h, const void* bytes,
                                     size_t n) {
  if (!n) return;
  const uint8_t* p = bytes;
  h->length += n;
  if (h->used) {
    size_t room = 64 - h->used, part = n < room ? n : room;
    memcpy(h->block + h->used, p, part);
    h->used += part;
    p += part;
    n -= part;
    if (h->used < 64) return;
    rmr_sha256_blocks(h->state, h->block, 1);
    h->used = 0;
  }
  rmr_sha256_blocks(h->state, p, n / 64);
  p += n / 64 * 64;
  n %= 64;
  if (n) memcpy(h->block, p, n);
  h->used = n;
}

/* The digest of everything taken; h is spent. */
static inline rmr_sha256_digest rmr_sha256_final(rmr_sha256* h) {
  uint64_t bits = h->length * 8;
  uint8_t pad = 0x80;
  rmr_sha256_update(h, &pad, 1);
  pad = 0;
  while (h->used != 56) rmr_sha256_update(h, &pad, 1);
  uint8_t tail[8];
  for (unsigned i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - 8 * i));
  rmr_sha256_update(h, tail, sizeof tail);
  rmr_sha256_digest d;
  for (unsigned i = 0; i < 8; i++)
    for (unsigned k = 0; k < 4; k++)
      d.bytes[4 * i + k] = (uint8_t)(h->state[i] >> (24 - 8 * k));
  return d;
}

/* The digest of size bytes at once. */
static inline rmr_sha256_digest rmr_sha256_of(const void* bytes, size_t size) {
  rmr_sha256 h;
  rmr_sha256_init(&h);
  rmr_sha256_update(&h, bytes, size);
  return rmr_sha256_final(&h);
}

#endif
