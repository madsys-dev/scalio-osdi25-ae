// city.h - cityhash-c
// CityHash on C
// Copyright (c) 2011-2012, Alexander Nusov
//
// - original copyright notice -
// Copyright (c) 2011 Google, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// CityHash, by Geoff Pike and Jyrki Alakuijala
//
// This file provides a few functions for hashing strings. On x86-64
// hardware in 2011, CityHash64() is faster than other high-quality
// hash functions, such as Murmur.  This is largely due to higher
// instruction-level parallelism.  CityHash64() and CityHash128() also perform
// well on hash-quality tests.
//
// CityHash128() is optimized for relatively long strings and returns
// a 128-bit hash.  For strings more than about 2000 bytes it can be
// faster than CityHash64().
//
// Functions in the CityHash family are not suitable for cryptography.
//
// WARNING: This code has not been tested on big-endian platforms!
// It is known to work well on little-endian platforms that have a small penalty
// for unaligned reads, such as current Intel and AMD moderate-to-high-end CPUs.
//
// By the way, for some hash functions, given strings a and b, the hash
// of a+b is easily derived from the hashes of a and b.  This property
// doesn't hold for any hash functions in this file.

#ifndef CITY_HASH_H_
#define CITY_HASH_H_

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stdlib.h>

typedef uint8_t uint8;
typedef uint32_t uint32;
typedef uint64_t uint64;

typedef struct _uint128 uint128;
struct _uint128 {
    uint64 first;
    uint64 second;
};

#define Uint128Low64(x) (x).first
#define Uint128High64(x) (x).second

// Hash function for a byte array.
uint64 CityHash64(const char *buf, size_t len);

// Hash function for a byte array.  For convenience, a 64-bit seed is also
// hashed into the result.
uint64 CityHash64WithSeed(const char *buf, size_t len, uint64 seed);

// Hash function for a byte array.  For convenience, two seeds are also
// hashed into the result.
uint64 CityHash64WithSeeds(const char *buf, size_t len, uint64 seed0, uint64 seed1);

// Hash function for a byte array.
uint128 CityHash128(const char *s, size_t len);

// Hash function for a byte array.  For convenience, a 128-bit seed is also
// hashed into the result.
uint128 CityHash128WithSeed(const char *s, size_t len, uint128 seed);

#ifdef __cplusplus
}
#endif
#endif  // CITY_HASH_H_