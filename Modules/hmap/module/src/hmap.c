/****************************************************************
 *
 *        Copyright 2013, Big Switch Networks, Inc.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 ****************************************************************/

#include "hmap_int.h"
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <AIM/aim.h>

/*
 * Comments on the overall datastructure are in hmap_int.h
 */

#define HMAP_INITIAL_SIZE 8
#define HMAP_DEFAULT_LOAD_FACTOR 0.8

struct hmap *
hmap_create(hmap_hash_f hash, hmap_equals_f equals,
            uint32_t key_offset, float max_load_factor)
{
    struct hmap *hmap = malloc(sizeof(*hmap));
    AIM_TRUE_OR_DIE(hmap != NULL, "hmap allocation failed");

    hmap->hash = hash;
    hmap->equals = equals;
    hmap->key_offset = key_offset;
    if (max_load_factor == 0.0f) {
        hmap->max_load_factor = HMAP_DEFAULT_LOAD_FACTOR;
    } else {
        hmap->max_load_factor = max_load_factor;
    }

    hmap->count = 0;
    hmap->size = HMAP_INITIAL_SIZE;
    hmap->threshold = hmap->size * hmap->max_load_factor;

    hmap->hashes = calloc(hmap->size, sizeof(*hmap->hashes));
    hmap->objects = malloc(hmap->size * sizeof(*hmap->objects));
    AIM_TRUE_OR_DIE(hmap->hashes != NULL && hmap->objects != NULL, "hmap allocation failed");

    return hmap;
}

uint32_t
hmap_count(const struct hmap *hmap)
{
    return hmap->count;
}

static uint32_t
hmap_index(const struct hmap *hmap, uint32_t hash, uint32_t distance)
{
    return (hash + distance) & (hmap->size - 1);
}

/*
 * Calculate how far this hash value is from its desired bucket
 */
static uint32_t
hmap_distance(const struct hmap *hmap, uint32_t idx, uint32_t hash)
{
    uint32_t start_idx = hmap_index(hmap, hash, 0);
    return (idx + hmap->size - start_idx) & (hmap->size - 1);
}

/*
 * Return a pointer to the key embedded in 'object'
 */
static const void *
hmap_object_key(const struct hmap *hmap, const void *object)
{
    return (const void *)((const char *)object + hmap->key_offset);
}

/*
 * Call the user-supplied hash function and munge the result to
 * not conflict with the special hash codes.
 */
static uint32_t
hmap_calc_hash(const struct hmap *hmap, const void *key)
{
    uint32_t hash = hmap->hash(key);
    hash &= ~HMAP_HASH_DELETED_BIT;
    hash |= hash == HMAP_HASH_FREE;
    return hash;
}

void *
hmap_lookup(const struct hmap *hmap, const void *key, uint32_t *state)
{
    uint32_t hash = hmap_calc_hash(hmap, key);
    uint32_t distance = 0;

    if (state != NULL) {
        distance = *state;
    } else {
        distance = 0;
    }

    for (; distance < hmap->size; distance++) {
        uint32_t idx = hmap_index(hmap, hash, distance);
        uint32_t bucket_hash = hmap->hashes[idx];
        if (bucket_hash == hash) {
            void *object = hmap->objects[idx];
            if (hmap->equals(key, hmap_object_key(hmap, object))) {
                if (state != NULL) {
                    *state = distance + 1;
                }
                return object;
            }
        } else if (bucket_hash == HMAP_HASH_FREE
                   || hmap_distance(hmap, idx, bucket_hash) < distance) {
            break;
        }
    }

    return NULL;
}

/*
 * Helper function for hmap_insert() and hmap_grow(). Inserts using the given
 * hash code without growing.
 */
static void
hmap_insert__(struct hmap *hmap, void *object, uint32_t hash)
{
    uint32_t distance;

    for (distance = 0; distance < hmap->size; distance++) {
        uint32_t idx = hmap_index(hmap, hash, distance);
        uint32_t bucket_hash = hmap->hashes[idx];
        uint32_t bucket_distance = hmap_distance(hmap, idx, bucket_hash);
        bool should_steal = distance > bucket_distance;

        if (bucket_hash == HMAP_HASH_FREE
            || ((bucket_hash & HMAP_HASH_DELETED_BIT) && should_steal)) {
            hmap->hashes[idx] = hash;
            hmap->objects[idx] = object;
            hmap->count++;
            return;
        } else if (should_steal) {
            /*
             * Swap with the current bucket owner and keep going to find
             * a new bucket for it.
             */
            void *bucket_object = hmap->objects[idx];
            hmap->hashes[idx] = hash;
            hmap->objects[idx] = object;
            hash = bucket_hash;
            object = bucket_object;
            distance = bucket_distance;
        }
    }

    assert(0);
}

/*
 * Double the hashtable size.
 */
static void
hmap_grow(struct hmap *hmap)
{
    uint32_t *old_hashes = hmap->hashes;
    void **old_objects = hmap->objects;

    uint32_t old_size = hmap->size;

    hmap->count = 0;
    hmap->size *= 2;
    hmap->threshold = hmap->size * hmap->max_load_factor;

    hmap->hashes = calloc(hmap->size, sizeof(*hmap->hashes));
    hmap->objects = malloc(hmap->size * sizeof(*hmap->objects));
    AIM_TRUE_OR_DIE(hmap->hashes != NULL && hmap->objects != NULL, "hmap growth failed");

    uint32_t idx;
    for (idx = 0; idx < old_size; idx++) {
        uint32_t hash = old_hashes[idx];
        void *object = old_objects[idx];
        if ((hash != HMAP_HASH_FREE) && !(hash & HMAP_HASH_DELETED_BIT)) {
            hmap_insert__(hmap, object, hash);
        }
    }

    free(old_hashes);
    free(old_objects);
}

void
hmap_insert(struct hmap *hmap, void *object)
{
    assert(object != NULL);

    if (hmap->count >= hmap->threshold) {
        hmap_grow(hmap);
    }

    uint32_t hash = hmap_calc_hash(hmap, hmap_object_key(hmap, object));

    hmap_insert__(hmap, object, hash);
}

void
hmap_remove(struct hmap *hmap, const void *object)
{
    uint32_t hash = hmap_calc_hash(hmap, hmap_object_key(hmap, object));
    uint32_t distance;

    for (distance = 0; distance < hmap->size; distance++) {
        uint32_t idx = hmap_index(hmap, hash, distance);
        uint32_t bucket_hash = hmap->hashes[idx];
        if (bucket_hash == hash) {
            void *bucket_object = hmap->objects[idx];
            if (bucket_object == object) {
                hmap->hashes[idx] = hash | HMAP_HASH_DELETED_BIT;
                hmap->count--;
                return;
            }
        }
    }

    assert(0);
}

void
hmap_destroy(struct hmap *hmap)
{
    free(hmap->hashes);
    free(hmap->objects);
    free(hmap);
}

int compare_uint32(const void *_a, const void *_b)
{
    uint32_t a = *(const uint32_t *)_a;
    uint32_t b = *(const uint32_t *)_b;
    if (a < b) {
        return -1;
    } else if (a > b) {
        return 1;
    } else {
        return 0;
    }
}

void hmap_stats(struct hmap *hmap)
{
    uint32_t idx;
    double distance_sum = 0;
    double distance_squared_sum = 0;
    for (idx = 0; idx < hmap->size; idx++) {
        uint32_t bucket_hash = hmap->hashes[idx];
        if (bucket_hash != HMAP_HASH_FREE
            && !(bucket_hash & HMAP_HASH_DELETED_BIT)) {
            uint32_t dist = hmap_distance(hmap, idx, bucket_hash);
            distance_sum += dist;
            distance_squared_sum += dist * dist;
        }
    }

    fprintf(stderr, "count=%u size=%u load=%f\n", hmap->count, hmap->size, (double)hmap->count/hmap->size);
    fprintf(stderr, "memory consumption: %u kilobytes\n", (uint32_t)(hmap->size * (sizeof(uint32_t) + sizeof(void *)) / 1024));

    double mean = distance_sum / hmap->count;
    double variance = (distance_squared_sum - distance_sum*distance_sum/hmap->count)/hmap->count;
    fprintf(stderr, "mean=%f\n", mean);
    fprintf(stderr, "variance=%f\n", variance);
}

uint32_t
hmap_uint16_hash(const void *key)
{
    /* 32-bit MurmurHash3 finalizer */
    uint32_t h = *(const uint16_t *)key;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

int
hmap_uint16_equality(const void *key1, const void *key2)
{
    return *(const uint16_t *)key1 == *(const uint16_t *)key2;
}

uint32_t
hmap_uint32_hash(const void *key)
{
    /* 32-bit MurmurHash3 finalizer */
    uint32_t h = *(const uint32_t *)key;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

int
hmap_uint32_equality(const void *key1, const void *key2)
{
    return *(const uint32_t *)key1 == *(const uint32_t *)key2;
}

uint32_t
hmap_uint64_hash(const void *key)
{
    /* 64-bit MurmurHash3 finalizer */
    uint64_t h = *(const uint64_t *)key;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccd;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53;
    h ^= h >> 33;
    return h;
}

int
hmap_uint64_equality(const void *key1, const void *key2)
{
    return *(const uint64_t *)key1 == *(const uint64_t *)key2;
}
