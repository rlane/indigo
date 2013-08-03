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

/**
 * @file
 * @brief Implementation of generic flow table routines
 */

#include <OFStateManager/ofstatemanager_config.h>
#include <indigo/indigo.h>
#include <murmur/murmur.h>

#include "ofstatemanager_log.h"
#include "ft.h"

/* Unless directed otherwise, do basic sanity checking */
#if !defined(FT_NO_ERROR_CHECKING)
#define FT_ASSERT(cond) INDIGO_ASSERT(cond)
#else
#define FT_ASSERT(cond)
#endif

static biglist_t *out_port_list_populate_from_actions(of_list_action_t *actions);
static biglist_t *out_port_list_populate_from_instructions(of_list_instruction_t *instructions);
static indigo_error_t ft_flow_set_effects(ft_entry_t *entry, of_flow_modify_t *flow_mod);
static void ft_entry_link(ft_instance_t ft, ft_entry_t *entry);
static void ft_entry_unlink(ft_instance_t ft, ft_entry_t *entry);
static indigo_error_t ft_entry_setup(ft_entry_t *entry, indigo_flow_id_t id, of_flow_add_t *flow_add);
static void ft_entry_clear(ft_instance_t ft, ft_entry_t *entry);


/**
 * Determine if the given entry has port as an output port
 */
static inline int
entry_has_out_port(ft_entry_t *entry, of_port_no_t port)
{
    biglist_t *elt;
    of_port_no_t chk_port;

    for (elt = entry->output_ports; elt != NULL; elt = elt->next) {
        chk_port = (uintptr_t)elt->data;
        if (port == chk_port) {
            return 1;
        }
    }

    return 0;
}

/**
 * Determine if an entry's match agrees with the metamatch data
 * @param query The match information from the query
 * @param entry Pointer to the flow table entry being checked
 * @returns Boolean, true if entry matches meta_match data
 *
 * @fixme Is any additional canonicalization required for match data?
 */

int
ft_flow_meta_match(of_meta_match_t *query, ft_entry_t *entry)
{
    uint64_t mask;
    int rv = 0; /* Default is no match */

    if (FT_FLOW_STATE_IS_DELETED(entry->state)) return (rv);

    if ((mask = query->cookie_mask)) {
        if ((query->cookie & mask) != (entry->cookie & mask)) {
            return rv;
        }
    }

    if (query->table_id != TABLE_ID_ANY) {
        if (query->table_id != entry->table_id) {
            return rv;
        }
    }

    if (query->check_priority) {
        if (entry->priority != query->priority) {
            return rv;
        }
    }

    switch (query->mode) {
    case OF_MATCH_NON_STRICT:
        /* Check if the entry's match is more specific than the query's */
        if (!of_match_more_specific(&entry->match, &query->match)) {
            break;
        }
        if (query->out_port != OF_PORT_DEST_WILDCARD) {
            if (!entry_has_out_port(entry, query->out_port)) {
                break;
            }
        }
        rv = 1;
        break;
    case OF_MATCH_STRICT:
        if (!of_match_eq(&entry->match, &query->match)) {
            break;
        }
        if (query->out_port != OF_PORT_DEST_WILDCARD) {
            if (!entry_has_out_port(entry, query->out_port)) {
                break;
            }
        }
        rv = 1;
        break;
    case OF_MATCH_COOKIE_ONLY:
        /* Checked cookie above */
        rv = 1;
        break;
    case OF_MATCH_OVERLAP:
        if (!of_match_overlap(&entry->match, &query->match)) {
            break;
        }
        rv = 1;
        break;
    default:
        /* Internal error */
        AIM_LOG_ERROR("Internal error: unknown query mode %d", query->mode);
        break;
    }

    return rv;
}

/**
 * Hash an of_match_t
 */
static uint32_t
match_hash(const void *key)
{
    return murmur_hash(key, sizeof(of_match_t), 0);
}

/**
 * Check equality of two of_match_t's
 */
static int
match_equality(const void *key1, const void *key2)
{
    return memcmp(key1, key2, sizeof(of_match_t)) == 0;
}

/**
 * Create a flow table instance
 * @param config Pointer to configuration structure
 * @returns A handle for the flow table instance to be used
 * in all calls affecting this table instance
 *
 * If max_entries <= 0, use the default size
 *
 * The implementation of this function does not attempt to be abstract
 *
 */

ft_instance_t
ft_hash_create(ft_config_t *config)
{
    ft_instance_t ft;
    int bytes;
    int idx;

    if (config->max_entries <= 0) {
        LOG_ERROR("Hash flow table only supports fixed number of buckets");
        return NULL;
    }

    /* Allocate the flow table itself */
    ft = INDIGO_MEM_ALLOC(sizeof(*ft));
    if (ft == NULL) {
        LOG_ERROR("ERROR: Flow table (hash) creation failed");
        return NULL;
    }
    INDIGO_MEM_SET(ft, 0, sizeof(*ft));
    INDIGO_MEM_COPY(&ft->config,  config, sizeof(ft_config_t));

    list_init(&ft->free_list);
    list_init(&ft->all_list);

    /* Allocate and init the flow entries */
    bytes = sizeof(ft_entry_t) * config->max_entries;
    ft->flow_entries = INDIGO_MEM_ALLOC(bytes);
    if (ft->flow_entries == NULL) {
        LOG_ERROR("ERROR: Flow table (hash) creation failed");
        INDIGO_MEM_FREE(ft);
        return NULL;
    }
    INDIGO_MEM_SET(ft->flow_entries, 0, bytes);

    /* Put the flow entries on the free list */
    for (idx = 0; idx < config->max_entries; idx++) {
        list_push(&ft->free_list, &ft->flow_entries[idx].table_links);
    }

    ft->match_index = hindex_create(match_hash, match_equality,
                                offsetof(struct ft_entry_s, match), 0);

    ft->flow_id_index = hindex_create(hindex_uint64_hash, hindex_uint64_equality,
                                  offsetof(struct ft_entry_s, id), 0);

    ft->priority_index = hindex_create(hindex_uint16_hash, hindex_uint16_equality,
                                   offsetof(struct ft_entry_s, priority), 0);

    return ft;
}

/**
 * Add a flow entry to the table
 * @param ft The flow table handle
 * @param id The external flow identifier
 * @param flow_add The LOCI flow mod object resulting in the add
 * @param entry_p Output; pointer to place to store entry if successful
 *
 * If the an entry already exists, an error is returned.
 */

indigo_error_t
ft_hash_flow_add(ft_instance_t ft, indigo_flow_id_t id,
                 of_flow_add_t *flow_add, ft_entry_t **entry_p)
{
    ft_entry_t *entry;
    list_links_t *links;
    indigo_error_t rv;

    LOG_TRACE("Adding flow " INDIGO_FLOW_ID_PRINTF_FORMAT, id);

    /* If flow ID already exists, error. */
    if (ft_id_lookup(ft, id) != NULL) {
        return INDIGO_ERROR_EXISTS;
    }

    /* Grab an entry from the free list */
    if ((links = list_pop(&ft->free_list)) == NULL) {
        ++(ft->status.table_full_errors);
        return INDIGO_ERROR_RESOURCE;
    }
    entry = FT_ENTRY_CONTAINER(links, table);

    if ((rv = ft_entry_setup(entry, id, flow_add)) < 0) {
        return rv;
    }

    ft_entry_link(ft, entry);
    ft->status.adds += 1;
    ft->status.current_count += 1;

    if (entry_p != NULL) {
        *entry_p = entry;
    }

    return INDIGO_ERROR_NONE;
}

/**
 * Remove a specific flow entry from the table
 * @param ft The flow table handle
 * @param entry Pointer to the entry to be removed
 */

indigo_error_t
ft_hash_flow_delete(ft_instance_t ft, ft_entry_t *entry)
{
    LOG_TRACE("Delete rsn %d flow " INDIGO_FLOW_ID_PRINTF_FORMAT,
              entry->removed_reason, entry->id);

    if (entry->id == INDIGO_FLOW_ID_INVALID) {
        LOG_ERROR("Deleting invalid flow table entry");
        return INDIGO_ERROR_UNKNOWN;
    }

    /* Unlink from hash lists; clear entry; put it on the free list */
    ft_entry_unlink(ft, entry);
    ft_entry_clear(ft, entry);

    INDIGO_ASSERT(entry->state == FT_FLOW_STATE_FREE);
    list_push(&ft->free_list, &entry->table_links);
    ft->status.current_count -= 1;
    ft->status.deletes += 1;

    return INDIGO_ERROR_NONE;
}

/**
 * Remove a flow entry from the table indicated by flow ID
 * @param ft The flow table handle
 * @param id Flow ID of the entry to remove
 *
 * Just looks up the entry and calls the above.
 */

indigo_error_t
ft_hash_flow_delete_id(ft_instance_t ft,
                       indigo_flow_id_t id)
{
    ft_entry_t *entry;

    if ((entry = ft_id_lookup(ft, id)) == NULL) {
        LOG_VERBOSE("Delete: Failed to find flow "
                    INDIGO_FLOW_ID_PRINTF_FORMAT, id);
        return INDIGO_ERROR_NOT_FOUND;
    }
    return ft_hash_flow_delete(ft, entry);
}

/**
 * Delete a flow table instance and free resources
 * @param ft A handle for the flow table instance to be deleted
 *
 * Will call ft_entry_clear on all entries.
 *
 * Free underlying data structures
 */

void
ft_hash_delete(ft_instance_t ft)
{
    ft_entry_t *entry;
    list_links_t *cur, *next;

    if (ft == NULL) {
        return;
    }

    FT_ITER(ft, entry, cur, next) {
        ft_entry_unlink(ft, entry);
        ft_entry_clear(ft, entry);
    }

    if (ft->flow_entries != NULL) {
        INDIGO_MEM_FREE(ft->flow_entries);
        ft->flow_entries = NULL;
    }

    if (ft->flow_id_index != NULL) {
        hindex_destroy(ft->flow_id_index);
    }

    if (ft->priority_index != NULL) {
        hindex_destroy(ft->priority_index);
    }

    if (ft->match_index != NULL) {
        hindex_destroy(ft->match_index);
    }

    INDIGO_MEM_FREE(ft);
}

/**
 * Query the flow table and return the first match if found
 * @param ft Handle for a flow table instance
 * @param query The meta-match data for the query
 * @param entry_ptr (out) Pointer to where to store the result if found
 * @returns INDIGO_ERROR_NONE if found; otherwise INDIGO_ERROR_NOT_FOUND
 *
 * entry_ptr may be NULL; Normally this is called with priority checked.
 */

indigo_error_t
ft_flow_first_match(ft_instance_t instance,
                    of_meta_match_t *query,
                    ft_entry_t **entry_ptr)
{
    ft_entry_t *entry;
    list_links_t *cur, *next;

    if (query->mode == OF_MATCH_STRICT) {
        uint32_t state = 0;
        while ((entry = hindex_lookup(instance->match_index,
                                    &query->match, &state)) != NULL) {
            if (!FT_FLOW_STATE_IS_DELETED(entry->state) &&
                    ft_flow_meta_match(query, entry)) {
                if (entry_ptr) {
                    *entry_ptr = entry;
                }
                return INDIGO_ERROR_NONE;
            }
        }
    } else if (query->check_priority) { /* Iterate thru prio hash list */
        uint32_t state = 0;
        while ((entry = hindex_lookup(instance->priority_index,
                                    &query->priority, &state)) != NULL) {
            if (!FT_FLOW_STATE_IS_DELETED(entry->state) &&
                    ft_flow_meta_match(query, entry)) {
                if (entry_ptr) {
                    *entry_ptr = entry;
                }
                return INDIGO_ERROR_NONE;
            }
        }
    } else { /* Iterate thru whole list */
        FT_ITER(instance, entry, cur, next) {
            if (!FT_FLOW_STATE_IS_DELETED(entry->state) &&
                    ft_flow_meta_match(query, entry)) {
                if (entry_ptr) {
                    *entry_ptr = entry;
                }
                return INDIGO_ERROR_NONE;
            }
        }
    }

    return INDIGO_ERROR_NOT_FOUND;
}

/**
 * Query the flow table and return all matches
 * @param ft Handle for a flow table instance
 * @param query The meta-match data for the query
 * @returns A list with pointers to ft_entry_t
 *
 * @fixme Currently we don't/can't check for failed alloc in biglist.
 */

biglist_t *
ft_flow_query(ft_instance_t instance, of_meta_match_t *query)
{
    ft_entry_t *entry;
    list_links_t *cur, *next;
    biglist_t *list = NULL;
    int count = 0;

    if (query->mode == OF_MATCH_STRICT) {
        uint32_t state = 0;
        while ((entry = hindex_lookup(instance->match_index,
                                    &query->match, &state)) != NULL) {
            if (!FT_FLOW_STATE_IS_DELETED(entry->state) &&
                    ft_flow_meta_match(query, entry)) {
                list = biglist_prepend(list, (void *)entry);
                count += 1;
            }
        }
    } else if (query->check_priority) {
        /* Iterate thru prio hash list */
        uint32_t state = 0;
        while ((entry = hindex_lookup(instance->priority_index,
                                    &query->priority, &state)) != NULL) {
            if (!FT_FLOW_STATE_IS_DELETED(entry->state) &&
                    ft_flow_meta_match(query, entry)) {
                list = biglist_prepend(list, (void *)entry);
                count += 1;
            }
        }
    } else { /* Iterate thru whole list */
        FT_ITER(instance, entry, cur, next) {
            if (!FT_FLOW_STATE_IS_DELETED(entry->state) &&
                    ft_flow_meta_match(query, entry)) {
                list = biglist_prepend(list, (void *)entry);
                count += 1;
            }
        }
    }

    LOG_TRACE("Query generated %d entries", count);
    return list;
}


/*
 * Flowtable iterator task
 *
 * Several operations follow a pattern of iterating over the entire flowtable
 * to make calls to Forwarding. Since the flowtable can be very large we need
 * to break up the work to allow processing higher priority events in between.
 *
 * These functions wrap the SocketManager task API to provide a simple method
 * for iterating over the flowtable without delaying higher priority events.
 */

struct ft_iter_task_state {
    ft_iter_task_callback_f callback;
    void *cookie;
    ft_instance_t flowtable;
    of_meta_match_t query;
    int use_query;
    int idx;
};

static ind_soc_task_status_t
ft_iter_task_callback(void *cookie)
{
    struct ft_iter_task_state *state = cookie;

    do {
        if (state->idx == state->flowtable->config.max_entries) {
            /* Finished */
            state->callback(state->cookie, NULL);
            INDIGO_MEM_FREE(state);
            return IND_SOC_TASK_FINISHED;
        } else {
            ft_entry_t *entry = &state->flowtable->flow_entries[state->idx++];

            if (entry->state == FT_FLOW_STATE_FREE ||
                FT_FLOW_STATE_IS_DELETED(entry->state)) {
                continue;
            }

            if (state->use_query && !ft_flow_meta_match(&state->query, entry)) {
                continue;
            }

            state->callback(state->cookie, entry);
        }
    } while (!ind_soc_should_yield());

    return IND_SOC_TASK_CONTINUE;
}

/**
 * Spawn a task that iterates over the flowtable
 *
 * @param ft Handle for a flow table instance
 * @param query The meta-match data for the query (or NULL)
 * @param callback Function called for each flowtable entry
 * @returns An error code
 *
 * This function does not guarantee a consistent view of the
 * flowtable over the course of the task.
 *
 * This function does not use any indexes on the flowtable.
 *
 * The callback function will be called with a NULL entry argument at
 * the end of the iteration.
 *
 * Deleted entries are skipped.
 */

indigo_error_t
ft_spawn_iter_task(ft_instance_t instance,
                   of_meta_match_t *query,
                   ft_iter_task_callback_f callback,
                   void *cookie,
                   int priority)
{
    indigo_error_t rv;

    struct ft_iter_task_state *state = INDIGO_MEM_ALLOC(sizeof(*state));
    if (state == NULL) {
        return INDIGO_ERROR_RESOURCE;
    }

    state->callback = callback;
    state->cookie = cookie;
    state->flowtable = instance;
    state->idx = 0;

    if (query != NULL) {
        state->query = *query;
        state->use_query = 1;
    } else {
        state->use_query = 0;
    }

    rv = ind_soc_task_register(ft_iter_task_callback, state, priority);
    if (rv != INDIGO_ERROR_NONE) {
        INDIGO_MEM_FREE(state);
        return rv;
    }

    return INDIGO_ERROR_NONE;
}

/**
 * Populate a list of output ports from actions
 */

static biglist_t *
out_port_list_populate_from_actions(of_list_action_t *actions)
{
    of_action_t elt;
    of_action_output_t *output;
    int loop_rv;
    of_port_no_t port_no;
    biglist_t *bl = NULL;

    output = &elt.output;
    OF_LIST_ACTION_ITER(actions, &elt, loop_rv) {
        if (output->object_id == OF_ACTION_OUTPUT) {
            of_action_output_port_get(output, &port_no);
            bl = biglist_prepend(bl, (void*)((uintptr_t)(port_no)));
        }
    }

    return bl;
}

static biglist_t *
out_port_list_populate_from_instructions(of_list_instruction_t *instructions)
{
    of_instruction_t inst;
    int loop_rv;
    biglist_t *bl = NULL;

    OF_LIST_INSTRUCTION_ITER(instructions, &inst, loop_rv) {
        if (inst.header.object_id == OF_INSTRUCTION_APPLY_ACTIONS) {
            of_list_action_t actions;
            of_instruction_apply_actions_actions_bind(&inst.apply_actions, &actions);
            bl = biglist_append_list(bl, out_port_list_populate_from_actions(&actions));
        }
    }

    return bl;
}

/* Populate the output port list and effects */
static indigo_error_t
ft_flow_set_effects(ft_entry_t *entry,
                    of_flow_modify_t *flow_mod)
{
    if (flow_mod->version == OF_VERSION_1_0)
    {
        of_list_action_t *actions;
        if ((actions = of_flow_modify_actions_get(flow_mod)) == NULL) {
            LOG_ERROR("Could not get action list");
            return INDIGO_ERROR_RESOURCE;
        }
        biglist_free(entry->output_ports);
        of_list_action_delete(entry->effects.actions);
        entry->output_ports = out_port_list_populate_from_actions(actions);
        entry->effects.actions = actions;
    } else {
        of_list_instruction_t *instructions;
        if ((instructions = of_flow_modify_instructions_get(flow_mod)) == NULL) {
            LOG_ERROR("Could not get instruction list");
            return INDIGO_ERROR_RESOURCE;
        }
        biglist_free(entry->output_ports);
        of_list_instruction_delete(entry->effects.instructions);
        entry->output_ports = out_port_list_populate_from_instructions(instructions);
        entry->effects.instructions = instructions;
    }

    return INDIGO_ERROR_NONE;
}

/**
 * Link an entry into the appropriate lists for the FT
 */

static void
ft_entry_link(ft_instance_t ft, ft_entry_t *entry)
{
    if (ft == NULL || entry == NULL) {
        FT_ASSERT(!"ft_entry_link called with NULL ft or entry");
        return;
    }

    /* Link to full table iteration */
    list_push(&ft->all_list, &entry->table_links);

    hindex_insert(ft->flow_id_index, entry);
    hindex_insert(ft->priority_index, entry);
    hindex_insert(ft->match_index, entry);
}

/**
 * Unlink an entry into the appropriate lists for the FT
 */

static void
ft_entry_unlink(ft_instance_t ft, ft_entry_t *entry)
{
    if (ft == NULL || entry == NULL) {
        FT_ASSERT(!"ft_entry_unlink called with NULL ft or entry");
        return;
    }

    FT_ASSERT(!list_empty(&ft->all_list));

    /* Remove from full table iteration */
    list_remove(&entry->table_links);

    hindex_remove(ft->flow_id_index, entry);
    hindex_remove(ft->priority_index, entry);
    hindex_remove(ft->match_index, entry);
}

ft_entry_t *
ft_id_lookup(ft_instance_t ft, indigo_flow_id_t id)
{
    return hindex_lookup(ft->flow_id_index, &id, NULL);
}


indigo_error_t
ft_flow_modify_cookie(ft_instance_t instance, ft_entry_t *entry,
                      uint64_t cookie, uint64_t cookie_mask)
{
    entry->cookie = (entry->cookie & cookie_mask) | (cookie & cookie_mask);

    return INDIGO_ERROR_NONE;
}

indigo_error_t
ft_flow_modify_effects(ft_instance_t instance,
                       ft_entry_t *entry,
                       of_flow_modify_t *flow_mod)
{
    indigo_error_t err;

    LOG_TRACE("Modifying effects of entry " INDIGO_FLOW_ID_PRINTF_FORMAT,
              entry->id);

    err = ft_flow_set_effects(entry, flow_mod);
    if (err == INDIGO_ERROR_NONE) {
        instance->status.updates += 1;
    }

    return err;
}

indigo_error_t
ft_flow_clear_counters(ft_entry_t *entry, uint64_t *packets, uint64_t *bytes)
{
    if (packets) {
        *packets = entry->packets;
    }
    if (bytes) {
        *bytes = entry->bytes;
    }

    entry->packets = 0;
    entry->bytes = 0;

    /* @fixme Update last counter update/change? */

    return INDIGO_ERROR_NONE;
}

/**
 * Initialize the data for a flow entry that is being added
 *
 * @param entry Pointer to entry being initialized
 * @param id The flow ID to use
 * @param flow_add Pointer to the flow add object for the entry
 *
 * The list links are not modified by this call.
 */
static indigo_error_t
ft_entry_setup(ft_entry_t *entry, indigo_flow_id_t id, of_flow_add_t *flow_add)
{
    of_flow_add_t *dup;
    indigo_error_t err;

    INDIGO_ASSERT(entry->state == FT_FLOW_STATE_FREE);

    entry->id = id;
    dup = (of_flow_add_t *)of_object_dup(flow_add);
    if (dup == NULL) {
        return INDIGO_ERROR_RESOURCE;
    }

    entry->state = FT_FLOW_STATE_NEW;
    entry->queued_reqs = NULL;
    entry->flow_add = dup;
    if (of_flow_add_match_get(flow_add, &entry->match) < 0) {
        of_object_delete(dup);
        return INDIGO_ERROR_UNKNOWN;
    }
    of_flow_add_cookie_get(flow_add, &entry->cookie);
    of_flow_add_priority_get(flow_add, &entry->priority);
    of_flow_add_flags_get(flow_add, &entry->flags);
    of_flow_add_idle_timeout_get(flow_add, &entry->idle_timeout);
    of_flow_add_hard_timeout_get(flow_add, &entry->hard_timeout);

    err = ft_flow_set_effects(entry, flow_add);
    if (err != INDIGO_ERROR_NONE) {
        return err;
    }

    entry->insert_time = INDIGO_CURRENT_TIME;
    entry->last_counter_change = entry->insert_time;

    return INDIGO_ERROR_NONE;
}

void
ft_flow_mark_deleted(ft_instance_t ft, ft_entry_t *entry,
                     indigo_fi_flow_removed_t reason)
{
    if (FT_FLOW_STATE_IS_DELETED(entry->state)) {
        return;
    }

    entry->state = FT_FLOW_STATE_DELETE_MARKED;
    entry->removed_reason = reason;

    ft->status.pending_deletes += 1;
}

/**
 * Release the data associated with an entry
 *
 * @param entry Pointer to entry being initialized
 *
 * The list links are not modified by this call.
 *
 * The entry ID and state are updated and pending deletes count is
 * decremented.
 */
static void
ft_entry_clear(ft_instance_t ft, ft_entry_t *entry)
{
    if (entry->output_ports != NULL) {
        biglist_free(entry->output_ports);
        entry->output_ports = NULL;
    }
    if (entry->effects.actions != NULL) {
        of_list_action_delete(entry->effects.actions);
        entry->effects.actions = NULL;
    }
    if (entry->flow_add != NULL) {
        of_flow_add_delete(entry->flow_add);
        entry->flow_add = NULL;
    }

    biglist_free(entry->queued_reqs);
    entry->id = INDIGO_FLOW_ID_INVALID;

    /* If entry was being deleted, pending deletes gets decremented */
    if (FT_FLOW_STATE_IS_DELETED(entry->state)) {
        ft->status.pending_deletes -= 1;
    }
    entry->state = FT_FLOW_STATE_FREE;
}
