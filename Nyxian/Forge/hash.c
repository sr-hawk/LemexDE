#include "hash.h"

#include <string.h>

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME  1099511628211ULL

Hash hash_bytes(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    Hash h = FNV_OFFSET;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

Hash hash_str(const char *s) {
    return hash_bytes(s, strlen(s));
}

Hash hash_combine(Hash a, Hash b) {
    /* boost::hash_combine-style mixer */
    a ^= b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2);
    return a;
}
