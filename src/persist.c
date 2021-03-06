/*
 * Copyright 2018 Jakub Klama <jakub.klama@gmail.com>
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <glib.h>
#include <rpc/object.h>
#include <persist.h>
#include "internal.h"

static int
persist_create_collection(persist_db_t db, const char *name)
{
	rpc_auto_object_t col;

	col = rpc_object_pack("{v,[],{}}",
	    "created_at", rpc_date_create_from_current(),
	    "migrations",
	    "metadata");

	if (db->pdb_driver->pd_create_collection(db->pdb_arg, name) != 0)
		return (-1);

	if (db->pdb_driver->pd_save_object(db->pdb_arg, COLLECTIONS,
	    name, col) != 0)
		return (-1);

	return (0);
}

persist_db_t
persist_open(const char *path, const char *driver, rpc_object_t params)
{
	struct persist_db *db;

	db = g_malloc0(sizeof(*db));
	db->pdb_path = path;
	db->pdb_driver = persist_find_driver(driver);

	if (db->pdb_driver->pd_open(db) != 0) {
		g_free(db);
		return (NULL);
	}

	if (db->pdb_driver->pd_create_collection(db->pdb_arg,
	    COLLECTIONS) != 0) {
		g_free(db);
		return (NULL);
	}

	return (db);
}

void
persist_close(persist_db_t db)
{

	db->pdb_driver->pd_close(db);
}

persist_collection_t
persist_collection_get(persist_db_t db, const char *name, bool create)
{
	persist_collection_t result;
	rpc_auto_object_t col = NULL;

	if (db->pdb_driver->pd_get_object(db->pdb_arg, COLLECTIONS,
	    name, &col) != 0) {
		if (errno == ENOENT && create) {
			if (persist_create_collection(db, name) == 0)
				goto ok;
		}

		return (NULL);
	}

ok:
	result = g_malloc0(sizeof(*result));
	result->pc_db = db;
	result->pc_name = g_strdup(name);
	return (result);
}

bool
persist_collection_exists(persist_db_t db, const char *name)
{

	return (db->pdb_driver->pd_get_object(db->pdb_arg, COLLECTIONS,
	    name, NULL) == 0);
}

int
persist_collection_remove(persist_db_t db, const char *name)
{

	return (db->pdb_driver->pd_destroy_collection(db->pdb_arg, name));
}

rpc_object_t
persist_collection_get_metadata(persist_db_t db, const char *name)
{
	rpc_object_t result;

	if (db->pdb_driver->pd_get_object(db->pdb_arg, COLLECTIONS,
	    name, &result) != 0) {
		persist_set_last_error(ENOENT, "Collection not found");
		return (NULL);
	}

	return (rpc_dictionary_get_value(result, "metadata"));
}

int
persist_collection_set_metadata(persist_db_t db, const char *name,
    rpc_object_t metadata)
{
	rpc_object_t result;

	if (db->pdb_driver->pd_get_object(db->pdb_arg, COLLECTIONS,
	    name, &result) != 0) {
		persist_set_last_error(ENOENT, "Collection not found");
		return (-1);
	}

	rpc_dictionary_set_value(result, "metadata", metadata);
	return (db->pdb_driver->pd_save_object(db->pdb_arg, COLLECTIONS,
	    name, result));
}

void
persist_collection_close(persist_collection_t collection)
{

	g_free(collection->pc_name);
	g_free(collection);
}

void
persist_collections_apply(persist_db_t db, persist_collection_iter_t fn)
{
	void *iter;
	char *id;

	iter = db->pdb_driver->pd_query(db->pdb_arg, COLLECTIONS, NULL, NULL);

	for (;;) {
		if (db->pdb_driver->pd_query_next(iter, &id, NULL) != 0)
			return;

		if (id == NULL)
			return;

		fn(id);
	}
}

int
persist_add_index(persist_collection_t col, const char *name, const char *path)
{

	return (col->pc_db->pdb_driver->pd_add_index(col->pc_db->pdb_arg,
	    col->pc_name, name, path));
}


int
persist_drop_index(persist_collection_t col, const char *name)
{

	return (col->pc_db->pdb_driver->pd_drop_index(col->pc_db->pdb_arg,
	    col->pc_name, name));
}

rpc_object_t
persist_get(persist_collection_t col, const char *id)
{
	rpc_object_t result;

	if (col->pc_db->pdb_driver->pd_get_object(col->pc_db->pdb_arg,
	    col->pc_name, id, &result) != 0)
		return (NULL);

	if (rpc_get_type(result) != RPC_TYPE_DICTIONARY) {
		persist_set_last_error(EINVAL,
		    "A non-dictionary object returned");
		rpc_release_impl(result);
		return (NULL);
	}

	rpc_dictionary_set_string(result, "id", id);
	return (result);
}

persist_iter_t
persist_query(persist_collection_t col, rpc_object_t rules,
    persist_query_params_t params)
{
	struct persist_iter *iter;

	iter = g_malloc0(sizeof(*iter));
	iter->pi_col = col;
	iter->pi_arg = col->pc_db->pdb_driver->pd_query(
	    col->pc_db->pdb_arg, col->pc_name, rules, params);

	if (iter->pi_arg == NULL) {
		g_free(iter);
		return (NULL);
	}

	return (iter);
}

ssize_t
persist_count(persist_collection_t col, rpc_object_t filter)
{

	return (col->pc_db->pdb_driver->pd_count(col->pc_db->pdb_arg,
	    col->pc_name, filter));
}

int
persist_save(persist_collection_t col, rpc_object_t obj)
{
	const char *id;

	if (rpc_get_type(obj) != RPC_TYPE_DICTIONARY) {
		persist_set_last_error(EINVAL, "Not a dictionary");
		return (-1);
	}

	id = rpc_dictionary_get_string(obj, "id");
	if (id == NULL) {
		persist_set_last_error(EINVAL,
		    "'id' field not present or not a string");
		return (-1);
	}

	if (col->pc_db->pdb_driver->pd_save_object(col->pc_db->pdb_arg,
	    col->pc_name, id, obj) != 0)
		return (-1);

	return (0);
}

int
persist_save_many(persist_collection_t col, rpc_object_t objects)
{

	if (rpc_get_type(objects) != RPC_TYPE_ARRAY) {
		persist_set_last_error(EINVAL, "Not an array");
		return (-1);
	}

	if (col->pc_db->pdb_driver->pd_save_objects(col->pc_db->pdb_arg,
	    col->pc_name, objects) != 0)
		return (-1);

	return (0);
}

int
persist_start_transaction(persist_db_t db)
{

	return (db->pdb_driver->pd_start_tx(db->pdb_arg));
}


int
persist_commit_transaction(persist_db_t db)
{

	return (db->pdb_driver->pd_commit_tx(db->pdb_arg));
}

int
persist_rollback_transaction(persist_db_t db)
{

	return (db->pdb_driver->pd_rollback_tx(db->pdb_arg));
}


bool
persist_transaction_active(_Nonnull persist_db_t db)
{

	return (db->pdb_driver->pd_in_tx(db->pdb_arg));
}


int
persist_iter_next(persist_iter_t iter, rpc_object_t *result)
{
	char *id;

	if (result == NULL) {
		persist_set_last_error(EINVAL, "result must not be NULL");
		return (-1);
	}

	if (iter->pi_col->pc_db->pdb_driver->pd_query_next(iter->pi_arg,
	    &id, result) != 0)
		return (-1);

	if (id == NULL || *result == NULL)
		return (0);

	rpc_dictionary_set_string(*result, "id", id);
	g_free(id);
	return (0);
}

void
persist_iter_close(persist_iter_t iter)
{

	iter->pi_col->pc_db->pdb_driver->pd_query_close(iter->pi_arg);
	g_free(iter);
}

int
persist_delete(persist_collection_t col, const char *id)
{

	return (col->pc_db->pdb_driver->pd_delete_object(col->pc_db->pdb_arg,
	    col->pc_name, id));
}
