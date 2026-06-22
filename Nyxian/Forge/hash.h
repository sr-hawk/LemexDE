/*
 * hash.h — content addressing.
 *
 * FNV-1a 64-bit: tiny and dependency-free. Used to give events stable ids and
 * to key the build cache, so that "same inputs => same key => cached output"
 * is what makes the build idempotent. (A production system would use a
 * cryptographic hash like BLAKE3; FNV is fine for a single-device,
 * non-adversarial cache.)
 */
#ifndef FORGE_HASH_H
#define FORGE_HASH_H

#include <stdint.h>
#include <stddef.h>

typedef uint64_t Hash;

Hash hash_bytes(const void *data, size_t len);
Hash hash_str(const char *s);
Hash hash_combine(Hash a, Hash b);

#endif /* FORGE_HASH_H */
