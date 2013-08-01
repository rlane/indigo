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

#ifndef _HMAP_H
#define _HMAP_H

#include <stdint.h>

/**
 * Hash function pointer type
 *
 * Return a hash of the key.
 */
typedef uint32_t (*hmap_hash_f)(const void *key);

/**
 * Equality function pointer type
 *
 * Return true if the two keys are equal.
 */
typedef int (*hmap_equals_f)(const void *key1, const void *key2);

/**
 * Create a hmap
 *
 * 'hash', 'equals', and 'key_offset' define the key used by this hmap.
 * 'key_offset', which should generally be computed with offsetof(),
 * is the location of the key in the containing "object". The 'hash'
 * and 'equality' functions should agree such that two keys that are
 * equal have the same hash code.
 *
 * 'max_load_factor' is a number between 0 and 1 expressing the fraction
 * of the table that can be filled before it automatically grows.
 * Suggested values are between 0.75 and 0.875. The lower the load factor
 * the faster most operations will be, but the hmap will use more memory.
 * Passing 0 for 'max_load_factor' will use a good default.
 */
struct hmap *hmap_create(hmap_hash_f hash, hmap_equals_f equals,
                         uint32_t key_offset, float max_load_factor);

/**
 * Return the number of objects in a hmap
 */
uint32_t hmap_count(const struct hmap *hmap);

/**
 * Look up an object by 'key' in a hmap
 *
 * 'state' is an optional parameter used for iterating over multiple objects
 * with the same key. If given it should be initialized to 0 for the first
 * lookup. Calling hmap_lookup() again with the same state pointer will
 * return successive objects with the given key. NULL will be returned
 * when the iteration is finished. The hmap must not be modified during the
 * iteration.
 *
 * Returns a pointer to the object, or NULL if not found.
 */
void *hmap_lookup(const struct hmap *hmap, const void *key, uint32_t *state);

/**
 * Insert an object into a hmap
 */
void hmap_insert(struct hmap *hmap, void *object);

/**
 * Remove an object from a hmap
 *
 * The object must already exist in the hmap.
 */
void hmap_remove(struct hmap *hmap, const void *object);

/**
 * Destroy a hmap
 *
 * Does not free any objects still in the hmap.
 */
void hmap_destroy(struct hmap *hmap);

/**
 * Output stats for this hmap to stderr
 */
void hmap_stats(struct hmap *hmap);

/* Hash/equality functions for common datatypes */

uint32_t hmap_uint16_hash(const void *key);
int hmap_uint16_equality(const void *key1, const void *key2);
uint32_t hmap_uint32_hash(const void *key);
int hmap_uint32_equality(const void *key1, const void *key2);
uint32_t hmap_uint64_hash(const void *key);
int hmap_uint64_equality(const void *key1, const void *key2);

#endif
