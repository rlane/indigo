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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
#include "hmap_int.h"

struct obj {
    uint32_t key;
};

static struct obj *
make_obj(uint32_t key)
{
    struct obj *obj = malloc(sizeof(*obj));
    obj->key = key;
    return obj;
}

/*
 * Trivial hash function used to allow the test to deliberately cause
 * collisions.
 */
static uint32_t
hash_uint32(const void *key)
{
    return *(uint32_t *)key;
}

static void
test_basic(void)
{
    struct hmap *hmap = hmap_create(hash_uint32, hmap_uint32_equality,
                                    offsetof(struct obj, key), 0.875);
    assert(hmap_count(hmap) == 0);

    struct obj *obj1 = make_obj(1);

    hmap_insert(hmap, obj1);
    assert(hmap_lookup(hmap, &obj1->key, NULL) == obj1);
    assert(hmap_count(hmap) == 1);

    hmap_remove(hmap, obj1);
    assert(hmap_lookup(hmap, &obj1->key, NULL) == NULL);
    assert(hmap_count(hmap) == 0);

    free(obj1);

    hmap_destroy(hmap);
}

/* qsort comparator */
static int compare_ptr(const void *_a, const void *_b)
{
    const void *a = *(const void **)_a;
    const void *b = *(const void **)_b;
    if (a < b) {
        return -1;
    } else if (a > b) {
        return 1;
    } else {
        return 0;
    }
}

/* Test multiple objects with the same key */
static void
test_multi(void)
{
    uint32_t key = 1;
    const int n = 3;
    int i;

    struct hmap *hmap = hmap_create(hash_uint32, hmap_uint32_equality,
                                    offsetof(struct obj, key), 0.875);
    assert(hmap_count(hmap) == 0);

    struct obj objs[n];

    for (i = 0; i < n; i++) {
        objs[i].key = key;
        hmap_insert(hmap, &objs[i]);
    }

    assert((int)hmap_count(hmap) == n);

    uint32_t state = 0;
    struct obj *results[n];
    for (i = 0; i < n; i++) {
        results[i] = hmap_lookup(hmap, &key, &state);
        assert(results[i] != NULL);
    }

    qsort(results, n, sizeof(*results), compare_ptr);
    for (i = 0; i < n; i++) {
        assert(results[i] == &objs[i]);
    }

    for (i = 0; i < n; i++) {
        hmap_remove(hmap, &objs[i]);
    }

    assert(hmap_count(hmap) == 0);

    hmap_destroy(hmap);
}

static void
test_fill(void)
{
    uint32_t key;
    const uint32_t n = 1024*10;

    struct hmap *hmap = hmap_create(hash_uint32, hmap_uint32_equality,
                                    offsetof(struct obj, key), 0.875);

    struct obj *objs = malloc(sizeof(*objs) * n);

    for (key = 0; key < n; key++) {
        objs[key].key = key;
    }

    for (key = 0; key < n; key++) {
        assert(hmap_lookup(hmap, &key, NULL) == NULL);
        hmap_insert(hmap, &objs[key]);
        assert(hmap_lookup(hmap, &key, NULL) == &objs[key]);
        assert(hmap_count(hmap) == key + 1);
    }

    for (key = 0; key < n; key++) {
        assert(hmap_lookup(hmap, &key, NULL) == &objs[key]);
    }

    assert(key == n);
    assert(hmap_lookup(hmap, &key, NULL) == NULL);

    for (key = 0; key < n; key++) {
        assert(hmap_lookup(hmap, &key, NULL) == &objs[key]);
        hmap_remove(hmap, &objs[key]);
        assert(hmap_lookup(hmap, &key, NULL) == NULL);
        assert(hmap_count(hmap) == n - key - 1);
    }

    free(objs);

    hmap_destroy(hmap);
}

static void
test_collisions(void)
{
    uint32_t key;

    struct hmap *hmap = hmap_create(hash_uint32, hmap_uint32_equality,
                                    offsetof(struct obj, key), 0.875);

    struct obj *obj1 = make_obj(1);
    struct obj *obj9 = make_obj(9);
    struct obj *obj2 = make_obj(2);

    hmap_insert(hmap, obj1);
    hmap_insert(hmap, obj9); /* collides with 1 */
    hmap_insert(hmap, obj2); /* bucket taken by 9 */
    assert(hmap_count(hmap) == 3);

    /* Dig into internals to validate collision handling */
    assert(hmap->objects[1] == obj1);
    assert(hmap->objects[2] == obj9);
    assert(hmap->objects[3] == obj2);

    key = 1; assert(hmap_lookup(hmap, &key, NULL) == obj1);
    key = 9; assert(hmap_lookup(hmap, &key, NULL) == obj9);
    key = 2; assert(hmap_lookup(hmap, &key, NULL) == obj2);

    hmap_remove(hmap, obj1);
    hmap_remove(hmap, obj9);
    hmap_remove(hmap, obj2);
    assert(hmap_count(hmap) == 0);

    free(obj1);
    free(obj9);
    free(obj2);

    hmap_destroy(hmap);
}

/* Test stealing bucket from another object */
static void
test_robin_hood(void)
{
    uint32_t key;

    struct hmap *hmap = hmap_create(hash_uint32, hmap_uint32_equality,
                                    offsetof(struct obj, key), 0.875);

    struct obj *obj1 = make_obj(1);
    struct obj *obj2 = make_obj(2);
    struct obj *obj9 = make_obj(9);

    hmap_insert(hmap, obj1); /* bucket 1, distance 0 */
    assert(hmap->objects[1] == obj1);

    hmap_insert(hmap, obj2); /* bucket 2, distance 0 */
    assert(hmap->objects[2] == obj2);

    hmap_insert(hmap, obj9); /* bucket 2, distance 1 */
    /* displaces obj2 */
    assert(hmap->objects[2] == obj9);
    assert(hmap->objects[3] == obj2);

    key = 1; assert(hmap_lookup(hmap, &key, NULL) == obj1);
    key = 2; assert(hmap_lookup(hmap, &key, NULL) == obj2);
    key = 9; assert(hmap_lookup(hmap, &key, NULL) == obj9);

    hmap_remove(hmap, obj1);
    hmap_remove(hmap, obj2);
    hmap_remove(hmap, obj9);

    free(obj1);
    free(obj2);
    free(obj9);

    hmap_destroy(hmap);
}

/* Test scenario where an object in the middle of a hash chain is deleted */
static void
test_robin_hood_deleted(void)
{
    uint32_t key;

    struct hmap *hmap = hmap_create(hash_uint32, hmap_uint32_equality,
                                    offsetof(struct obj, key), 0.875);

    struct obj *obj1 = make_obj(1);
    struct obj *obj2 = make_obj(2);
    struct obj *obj9 = make_obj(9);
    struct obj *obj17 = make_obj(17);

    hmap_insert(hmap, obj1); /* bucket 1, distance 0 */
    assert(hmap->objects[1] == obj1);

    hmap_insert(hmap, obj9); /* bucket 2, distance 1 */
    assert(hmap->objects[2] == obj9);

    hmap_insert(hmap, obj17); /* bucket 3, distance 2 */
    assert(hmap->objects[3] == obj17);

    key = 1; assert(hmap_lookup(hmap, &key, NULL) == obj1);
    key = 9; assert(hmap_lookup(hmap, &key, NULL) == obj9);
    key = 17; assert(hmap_lookup(hmap, &key, NULL) == obj17);

    /* Replace bucket 2 with a tombstone */
    hmap_remove(hmap, obj9);

    key = 1; assert(hmap_lookup(hmap, &key, NULL) == obj1);
    key = 17; assert(hmap_lookup(hmap, &key, NULL) == obj17);

    /*
     * obj2 is not allowed in bucket 2 because that would disrupt the
     * bucket 0 chain.
     */
    hmap_insert(hmap, obj2); /* bucket 4, distance 2 */
    assert(hmap->objects[4] == obj2);

    key = 1; assert(hmap_lookup(hmap, &key, NULL) == obj1);
    key = 2; assert(hmap_lookup(hmap, &key, NULL) == obj2);
    key = 17; assert(hmap_lookup(hmap, &key, NULL) == obj17);

    hmap_remove(hmap, obj1);
    hmap_remove(hmap, obj2);
    hmap_remove(hmap, obj17);

    free(obj1);
    free(obj2);
    free(obj9);
    free(obj17);

    hmap_destroy(hmap);
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    test_basic();
    test_multi();
    test_fill();
    test_collisions();
    test_robin_hood();
    test_robin_hood_deleted();
    return 0;
}
