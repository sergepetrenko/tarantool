/*
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <assert.h>
#include <stdbool.h>
#include <math.h> /* modf, isfinite */
#include <lua.h>
#include <lauxlib.h>

#include "lua/serializer.h"

#include "trigger.h"
#include "lib/core/decimal.h" /* decimal_t */
#include "lib/core/mp_extension_types.h"
#include "lua/error.h"

#include "datetime.h"
#include "trivia/util.h"
#include "diag.h"
#include "lua/utils.h"
#include "tt_static.h"

int luaL_map_metatable_ref = LUA_REFNIL;
int luaL_array_metatable_ref = LUA_REFNIL;
extern uint32_t CTID_UUID;
extern uint32_t CTID_DECIMAL;

/* {{{ luaL_serializer manipulations */

#define OPTION(type, name, defvalue) { #name, \
	offsetof(struct luaL_serializer, name), type, defvalue}
/**
 * Configuration options for serializers
 * @sa struct luaL_serializer
 */
static struct {
	const char *name;
	size_t offset; /* offset in structure */
	int type;
	int defvalue;
} OPTIONS[] = {
	OPTION(LUA_TBOOLEAN, encode_sparse_convert, 1),
	OPTION(LUA_TNUMBER,  encode_sparse_ratio, 2),
	OPTION(LUA_TNUMBER,  encode_sparse_safe, 10),
	OPTION(LUA_TNUMBER,  encode_max_depth, 128),
	OPTION(LUA_TBOOLEAN, encode_deep_as_nil, 0),
	OPTION(LUA_TBOOLEAN, encode_invalid_numbers, 1),
	OPTION(LUA_TNUMBER,  encode_number_precision, 14),
	OPTION(LUA_TBOOLEAN, encode_load_metatables, 1),
	OPTION(LUA_TBOOLEAN, encode_use_tostring, 0),
	OPTION(LUA_TBOOLEAN, encode_invalid_as_nil, 0),
	OPTION(LUA_TBOOLEAN, encode_error_as_ext, 1),
	OPTION(LUA_TTABLE,   encode_key_order, 0),
	OPTION(LUA_TBOOLEAN, decode_invalid_numbers, 1),
	OPTION(LUA_TBOOLEAN, decode_save_metatables, 1),
	OPTION(LUA_TNUMBER,  decode_max_depth, 128),
	{ NULL, 0, 0, 0},
};

void
luaL_serializer_create(struct luaL_serializer *cfg)
{
	rlist_create(&cfg->on_update);
	for (int i = 0; OPTIONS[i].name != NULL; i++) {
		switch (OPTIONS[i].type) {
		case LUA_TBOOLEAN:
		case LUA_TNUMBER: {
			int *pval = (int *)((char *)cfg + OPTIONS[i].offset);
			*pval = OPTIONS[i].defvalue;
			break;
		}
		case LUA_TTABLE: {
			if (strcmp(OPTIONS[i].name, "encode_key_order") != 0) {
				unreachable();
			}

			char **p = (char **)((char *)cfg + OPTIONS[i].offset);
			*p = NULL;
			break;
		}
		default:
			unreachable();
		}
	}
}

void
luaL_serializer_copy_options(struct luaL_serializer *dst,
			     const struct luaL_serializer *src)
{
	luaL_serializer_options_delete(dst);
	memcpy(dst, src, offsetof(struct luaL_serializer, end_of_options));

	/*
	 * Do a deep copy of the encoder key order. Not doing so would lead to
	 * double free.
	 */
	size_t encode_key_order_len = 0;
	while (src->encode_key_order != NULL &&
	       src->encode_key_order[encode_key_order_len] != NULL) {
		encode_key_order_len++;
	}

	if (encode_key_order_len > 0) {
		dst->encode_key_order = xcalloc(
			encode_key_order_len + 1, sizeof(char *));

		for (size_t i = 0; i < encode_key_order_len; i++) {
			size_t len = strlen(src->encode_key_order[i]);
			dst->encode_key_order[i] = xcalloc(len + 1,
							   sizeof(char));
			memcpy(dst->encode_key_order[i],
			       src->encode_key_order[i],
			       len);
		}
	} else {
		dst->encode_key_order = NULL;
	}
}

/**
 * Free memory allocated for encode_key_order.
 * @param cfg Serializer configuration.
 */
static void
luaL_serializer_free_encode_key_order(struct luaL_serializer *cfg)
{
	if (cfg->encode_key_order == NULL) {
		return;
	}

	for (size_t i = 0; cfg->encode_key_order[i] != NULL; i++) {
		free(cfg->encode_key_order[i]);
	}

	free(cfg->encode_key_order);
	cfg->encode_key_order = NULL;
}

void
luaL_serializer_options_delete(struct luaL_serializer *cfg)
{
	assert(cfg);
	luaL_serializer_free_encode_key_order(cfg);
	/* This is a partial TRASH(cfg), only for options. */
	memset(cfg, '#', offsetof(struct luaL_serializer, end_of_options));
}

/**
 * Destroy the serializer object, freeing all allocated memory.
 * @param L Lua stack.
 * @retval 0.
 */
static int
luaL_serializer_destroy(struct lua_State *L)
{
	struct luaL_serializer *cfg = lua_touserdata(L, 1);
	luaL_serializer_options_delete(cfg);
	return 0;
}

/**
 * Parse encode_key_order table from stack into serializer config.
 * @param L Lua stack.
 * @param cfg Serializer configuration.
 */
static void
luaL_serializer_parse_encode_key_order(struct lua_State *L,
				       struct luaL_serializer *cfg)
{
	assert(lua_istable(L, -1) || lua_isnil(L, -1) || luaL_isnull(L, -1));

	luaL_serializer_free_encode_key_order(cfg);

	size_t key_order_len = lua_objlen(L, -1);
	if (key_order_len > 0) {
		cfg->encode_key_order = xcalloc(
			key_order_len + 1, sizeof(char *));
	}

	for (size_t i = 0, j = 0; i < key_order_len; i++) {
		lua_pushinteger(L, i + 1);
		lua_gettable(L, -2);

		/* Skip keys that are not strings or numbers. */
		if (!lua_isstring(L, -1)) {
			lua_pop(L, 1);
			continue;
		}

		size_t len = 0;
		const char *key = lua_tolstring(L, -1, &len);
		cfg->encode_key_order[j] = xcalloc(len + 1, sizeof(char));
		memcpy(cfg->encode_key_order[j], key, len);
		lua_pop(L, 1);
		j++;
	}
}

/**
 * Configure one field in @a cfg. Value of the field is kept on
 * Lua stack after this function, and should be popped manually.
 * @param L Lua stack.
 * @param i Index of option in OPTIONS[].
 * @param cfg Serializer to inherit configuration.
 * @retval Pointer to the value of option.
 * @retval NULL if option is not in the table.
 */
static int *
luaL_serializer_parse_option(struct lua_State *L, int i,
			     struct luaL_serializer *cfg)
{
	lua_getfield(L, -1, OPTIONS[i].name);
	if (lua_isnil(L, -1))
		return NULL;
	/*
	 * Update struct luaL_serializer using pointer to a
	 * configuration value (all values must be `int` for that).
	*/
	int *pval = (int *) ((char *) cfg + OPTIONS[i].offset);
	switch (OPTIONS[i].type) {
	case LUA_TBOOLEAN:
		*pval = lua_toboolean(L, -1);
		break;
	case LUA_TNUMBER:
		*pval = lua_tointeger(L, -1);
		break;
	case LUA_TTABLE:
		if (strcmp(OPTIONS[i].name, "encode_key_order") == 0) {
			luaL_serializer_parse_encode_key_order(L, cfg);
			break;
		}
		unreachable();
	default:
		unreachable();
	}
	return pval;
}

void
luaL_serializer_parse_options(struct lua_State *L,
			      struct luaL_serializer *cfg)
{
	for (int i = 0; OPTIONS[i].name != NULL; ++i) {
		luaL_serializer_parse_option(L, i, cfg);
		lua_pop(L, 1);
	}
}

/**
 * @brief serializer.cfg{} Lua binding for serializers.
 * serializer.cfg is a table that contains current configuration values from
 * luaL_serializer structure. serializer.cfg has overriden __call() method
 * to change configuration keys in internal userdata (like box.cfg{}).
 * Please note that direct change in serializer.cfg.key will not affect
 * internal state of userdata. Changes via cfg() are reflected in
 * both Lua cfg table, and C serializer structure.
 * @param L lua stack
 * @return 0
 */
static int
luaL_serializer_cfg(struct lua_State *L)
{
	/* Serializer.cfg */
	luaL_checktype(L, 1, LUA_TTABLE);
	/* Updated parameters. */
	luaL_checktype(L, 2, LUA_TTABLE);
	struct luaL_serializer *cfg = luaL_checkserializer(L);
	for (int i = 0; OPTIONS[i].name != NULL; ++i) {
		if (luaL_serializer_parse_option(L, i, cfg) == NULL)
			lua_pop(L, 1);
		else
			lua_setfield(L, 1, OPTIONS[i].name);
	}
	trigger_run(&cfg->on_update, cfg);
	return 0;
}

void
luaL_push_encode_key_order(struct lua_State *L, struct luaL_serializer *cfg)
{
	if (cfg->encode_key_order == NULL) {
		lua_pushnil(L);
		return;
	}

	lua_newtable(L);
	for (size_t i = 0; cfg->encode_key_order[i] != NULL; i++) {
		lua_pushinteger(L, i + 1);
		lua_pushstring(L, cfg->encode_key_order[i]);
		lua_settable(L, -3);
	}
}

struct luaL_serializer *
luaL_newserializer_config(struct lua_State *L)
{
	struct luaL_serializer *serializer = (struct luaL_serializer *)
		lua_newuserdata(L, sizeof(*serializer));
	luaL_getmetatable(L, LUAL_SERIALIZER);
	lua_setmetatable(L, -2);
	luaL_serializer_create(serializer);
	return serializer;
}

struct luaL_serializer *
luaL_newserializer(struct lua_State *L, const char *modname,
		   const luaL_Reg *reg)
{
	luaL_checkstack(L, 1, "too many upvalues");

	/* Create new module */
	lua_newtable(L);

	/* Create new configuration */
	struct luaL_serializer *serializer = luaL_newserializer_config(L);

	for (; reg->name != NULL; reg++) {
		/* push luaL_serializer as upvalue */
		lua_pushvalue(L, -1);
		/* register method */
		lua_pushcclosure(L, reg->func, 1);
		lua_setfield(L, -3, reg->name);
	}

	/* Add cfg{} */
	lua_newtable(L); /* cfg */
	lua_newtable(L); /* metatable */
	lua_pushvalue(L, -3); /* luaL_serializer */
	lua_pushcclosure(L, luaL_serializer_cfg, 1);
	lua_setfield(L, -2, "__call");
	lua_setmetatable(L, -2);
	/* Save configuration values to serializer.cfg */
	for (int i = 0; OPTIONS[i].name != NULL; i++) {
		int *pval = (int *) ((char *) serializer + OPTIONS[i].offset);
		switch (OPTIONS[i].type) {
		case LUA_TBOOLEAN:
			lua_pushboolean(L, *pval);
			break;
		case LUA_TNUMBER:
			lua_pushinteger(L, *pval);
			break;
		case LUA_TTABLE:
			if (strcmp(OPTIONS[i].name, "encode_key_order") == 0) {
				luaL_push_encode_key_order(L, serializer);
				break;
			}
			unreachable();
		default:
			unreachable();
		}
		lua_setfield(L, -2, OPTIONS[i].name);
	}
	lua_setfield(L, -3, "cfg");

	lua_pop(L, 1);  /* remove upvalues */

	luaL_pushnull(L);
	lua_setfield(L, -2, "NULL");
	lua_rawgeti(L, LUA_REGISTRYINDEX, luaL_array_metatable_ref);
	lua_setfield(L, -2, "array_mt");
	lua_rawgeti(L, LUA_REGISTRYINDEX, luaL_map_metatable_ref);
	lua_setfield(L, -2, "map_mt");

	if (modname != NULL) {
		/* Register module. */
		lua_pushvalue(L, -1);
		luaT_setmodule(L, modname);
	}

	return serializer;
}

/* }}} luaL_serializer manipulations */

/* {{{ Fill luaL_field */

static int
lua_gettable_wrapper(lua_State *L)
{
	lua_gettable(L, -2);
	return 1;
}

static int
lua_field_inspect_ucdata(struct lua_State *L, struct luaL_serializer *cfg,
			int idx, struct luaL_field *field)
{
	if (!cfg->encode_load_metatables)
		return 0;

	/*
	 * Try to call LUAL_SERIALIZE method on udata/cdata
	 * LuaJIT specific: lua_getfield/lua_gettable raises exception on
	 * cdata if field doesn't exist.
	 */
	int top = lua_gettop(L);
	lua_pushcfunction(L, lua_gettable_wrapper);
	lua_pushvalue(L, idx);
	lua_pushliteral(L, LUAL_SERIALIZE);
	if (lua_pcall(L, 2, 1, 0) == 0  && !lua_isnil(L, -1)) {
		if (!lua_isfunction(L, -1)) {
			diag_set(LuajitError,
				 "invalid " LUAL_SERIALIZE " value");
			return -1;
		}
		/* copy object itself */
		lua_pushvalue(L, idx);
		if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
			diag_set(LuajitError, lua_tostring(L, -1));
			return -1;
		}
		/* replace obj with the unpacked value */
		lua_replace(L, idx);
		if (luaL_tofield(L, cfg, idx, field) < 0)
			return -1;
	} /* else ignore lua_gettable exceptions */
	lua_settop(L, top); /* remove temporary objects */
	return 0;
}

/**
 * Call __serialize method of a table object by index
 * if the former exists.
 *
 * If __serialize does not exist then function does nothing
 * and the function returns 1;
 *
 * If __serialize exists, is a function (which doesn't
 * raise any error) then a result of serialization
 * replaces old value by the index and the function returns 0;
 *
 * If the serialization is a hint string (like 'array' or 'map'),
 * then field->type, field->size and field->compact
 * are set if necessary and the function returns 0;
 *
 * Otherwise it is an error, set diag and the funciton returns -1;
 *
 * Return values:
 * -1 - error occurs, diag is set, the top of guest stack
 *      is undefined.
 *  0 - __serialize field is available in the metatable,
 *      the result value is put in the origin slot,
 *      encoding is finished.
 *  1 - __serialize field is not available in the metatable,
 *      proceed with default table encoding.
 */
static int
lua_field_try_serialize(struct lua_State *L, struct luaL_serializer *cfg,
			int idx, struct luaL_field *field)
{
	if (luaL_getmetafield(L, idx, LUAL_SERIALIZE) == 0)
		return 1;
	if (lua_isfunction(L, -1)) {
		/* copy object itself */
		lua_pushvalue(L, idx);
		if (lua_pcall(L, 1, 1, 0) != 0) {
			diag_set(LuajitError, lua_tostring(L, -1));
			return -1;
		}
		if (luaL_tofield(L, cfg, -1, field) != 0)
			return -1;
		lua_replace(L, idx);
		return 0;
	}
	if (!lua_isstring(L, -1)) {
		diag_set(LuajitError, "invalid " LUAL_SERIALIZE " value");
		return -1;
	}
	const char *type = lua_tostring(L, -1);
	if (strcmp(type, "array") == 0 || strcmp(type, "seq") == 0 ||
	    strcmp(type, "sequence") == 0) {
		field->type = MP_ARRAY; /* Override type */
		field->size = luaL_arrlen(L, idx);
		/* YAML: use flow mode if __serialize == 'seq' */
		if (cfg->has_compact && type[3] == '\0')
			field->compact = true;
	} else if (strcmp(type, "map") == 0 || strcmp(type, "mapping") == 0) {
		field->type = MP_MAP;   /* Override type */
		field->size = luaL_maplen(L, idx);
		/* YAML: use flow mode if __serialize == 'map' */
		if (cfg->has_compact && type[3] == '\0')
			field->compact = true;
	} else {
		diag_set(LuajitError, "invalid " LUAL_SERIALIZE " value");
		return -1;
	}
	/* Remove value set by luaL_getmetafield. */
	lua_pop(L, 1);
	return 0;
}

static int
lua_field_inspect_table(struct lua_State *L, struct luaL_serializer *cfg,
			int idx, struct luaL_field *field)
{
	assert(lua_type(L, idx) == LUA_TTABLE);
	uint32_t size = 0;
	uint32_t max = 0;

	if (cfg->encode_load_metatables) {
		int top = lua_gettop(L);
		int res = lua_field_try_serialize(L, cfg, idx, field);
		if (res == -1)
			return -1;
		assert(lua_gettop(L) == top);
		(void)top;
		if (res == 0)
			return 0;
		/* Fallthrough with res == 1 */
	}

	field->type = MP_ARRAY;

	/* Calculate size and check that table can represent an array */
	lua_pushnil(L);
	while (lua_next(L, idx)) {
		size++;
		lua_pop(L, 1); /* pop the value */
		lua_Number k;
		if (lua_type(L, -1) != LUA_TNUMBER ||
		    ((k = lua_tonumber(L, -1)) != size &&
		     (k < 1 || floor(k) != k))) {
			/* Finish size calculation */
			while (lua_next(L, idx)) {
				size++;
				lua_pop(L, 1); /* pop the value */
			}
			field->type = MP_MAP;
			field->size = size;
			return 0;
		}
		if (k > max)
			max = k;
	}

	/* Encode excessively sparse arrays as objects (if enabled) */
	if (cfg->encode_sparse_ratio > 0 &&
	    max > size * (uint32_t)cfg->encode_sparse_ratio &&
	    max > (uint32_t)cfg->encode_sparse_safe) {
		if (!cfg->encode_sparse_convert) {
			diag_set(LuajitError, "excessively sparse array");
			return -1;
		}
		field->type = MP_MAP;
		field->size = size;
		return 0;
	}

	assert(field->type == MP_ARRAY);
	field->size = max;
	return 0;
}

static int
lua_field_tostring(struct lua_State *L, struct luaL_serializer *cfg, int idx,
		   struct luaL_field *field)
{
	int top = lua_gettop(L);
	lua_getglobal(L, "tostring");
	lua_pushvalue(L, idx);
	lua_call(L, 1, 1);
	lua_replace(L, idx);
	lua_settop(L, top);
	return luaL_tofield(L, cfg, idx, field);
}

/**
 * Get the next field from the leftovers table in unsorted order.
 * @param L Lua stack.
 * @retval 0 if there are no more fields in the table.
 */
static int
luaL_next_field_unsorted(struct lua_State *L)
{
	assert(lua_gettop(L) >= 4);
	assert(lua_isnil(L, -2));
	assert(lua_istable(L, -3));
	assert(lua_istable(L, -4));

	/* const table, table, nil, key */
	int r = lua_next(L, -3);

	if (r == 0) {
		/*
		 * No more elements in the table. Delete the copy of the table.
		 * const table, table, nil
		 */
		lua_pop(L, 2);
		/* const table */
		return 0;
	}

	return r;
}

/**
 * Get the next field from the initial table in unsorted order.
 * @param L Lua stack.
 * @retval 0 if there are no more fields in the table.
 */
static int
luaL_next_field_fallback(struct lua_State *L)
{
	assert(lua_gettop(L) >= 3);
	assert(lua_isnil(L, -2));
	assert(lua_istable(L, -3));

	/* const table, nil, key */
	int r = lua_next(L, -3);

	if (r == 0) {
		/* const table, nil */
		lua_pop(L, 1);
		/* const table */
		return 0;
	}

	/* const table, nil, key, value */
	return r;
}

int
luaL_next_field(struct lua_State *L, struct luaL_serializer *cfg)
{
	/* const table, (table ?), key_idx, key */
	assert(lua_gettop(L) >= 3);
	assert(lua_isnil(L, -2) || lua_isnumber(L, -2));
	assert(lua_istable(L, -3));

	/* No need to sort anything */
	if (cfg->encode_key_order == NULL) {
		return luaL_next_field_fallback(L);
	}

	/*
	 * Both key_idx and key are nil, first call, copy the table
	 * so that we can remove printed elements from its copy, leaving
	 * the initial table intact.
	 */
	if (lua_isnil(L, -1) && lua_isnil(L, -2)) {
		/* const table, nil, nil */
		lua_pop(L, 2);
		lua_newtable(L);
		/* const table, table */

		/* Copy the table. */
		lua_pushnil(L);
		/* const table, table, nil */
		while (lua_next(L, -3)) {
			/* const table, table, key, value */
			lua_pushvalue(L, -2);
			/* const table, table, key, value, key */
			lua_pushvalue(L, -2);
			/* const table, table, key, value, key, value */
			lua_settable(L, -5);
			/* const table, table, key, value */
			lua_pop(L, 1);
			/* const table, table, key */
		}
		/* const table, table */
		lua_pushnil(L);
		lua_pushnil(L);
		/* const table, table, nil, nil */
	}

	/* Going through the unsorted part */
	if (!lua_isnil(L, -1) && lua_isnil(L, -2)) {
		return luaL_next_field_unsorted(L);
	}

	/**
	 * We are iterating through sorted part of the table
	 * don't need to know the key.
	 * const table, table, key_idx, key
	 */
	lua_pop(L, 1);
	/* const table, table, key_idx */

	assert(lua_isnil(L, -1) || lua_isnumber(L, -1));

	/* Start with the first key */
	lua_Integer key_idx = 0;
	if (lua_isnumber(L, -1)) {
		/* Get next key index if it is a number */
		key_idx = lua_tointeger(L, -1) + 1;
		assert(key_idx >= 0);
	}
	/* const table, table, key_idx */
	lua_pop(L, 1);
	/* const table, table */

	/* Find the key in the key order. */
	char *key = cfg->encode_key_order[key_idx];

	/* encode_key_order end, continue iterating over the unsorted part. */
	if (key == NULL) {
		/* const table, table */
		lua_pushnil(L);
		lua_pushnil(L);
		/* const table, table, nil, nil */
		return luaL_next_field_unsorted(L);
	}

	/* Push current key_idx, key and value. */
	/* const table, table */
	lua_pushinteger(L, key_idx);
	lua_pushstring(L, key);
	lua_getfield(L, -3, key);
	/* const table, table, key_idx, key, value */

	/* Remove the key from the table. */
	lua_pushnil(L);
	lua_setfield(L, -5, key);

	/* const table, table, key_idx, key, value */
	return 5;
}

int
luaL_tofield(struct lua_State *L, struct luaL_serializer *cfg, int index,
	     struct luaL_field *field)
{
	field->type = MP_NIL;
	field->ext_type = MP_UNKNOWN_EXTENSION;
	field->compact = false;

	if (index < 0)
		index = lua_gettop(L) + index + 1;

	double num;
	double intpart;
	size_t size;

#define CHECK_NUMBER(x) ({							\
	if (!isfinite(x) && !cfg->encode_invalid_numbers) {			\
		if (!cfg->encode_invalid_as_nil) {				\
			diag_set(LuajitError, "number must not be NaN or Inf");	\
			return -1;						\
		}								\
		field->type = MP_NIL;						\
	}})

	switch (lua_type(L, index)) {
	case LUA_TNUMBER:
		num = lua_tonumber(L, index);
		if (isfinite(num) && modf(num, &intpart) != 0.0) {
			field->type = MP_DOUBLE;
			field->dval = num;
		} else if (num >= 0 && num < exp2(64)) {
			field->type = MP_UINT;
			field->ival = (uint64_t) num;
		} else if (num >= -exp2(63) && num < exp2(63)) {
			field->type = MP_INT;
			field->ival = (int64_t) num;
		} else {
			field->type = MP_DOUBLE;
			field->dval = num;
			CHECK_NUMBER(num);
		}
		return 0;
	case LUA_TCDATA:
	{
		uint32_t ctypeid;
		void *cdata = luaL_tocpointer(L, index, &ctypeid);

		int64_t ival;
		switch (ctypeid) {
		case CTID_BOOL:
			field->type = MP_BOOL;
			field->bval = *(bool*) cdata;
			return 0;
		case CTID_CCHAR:
		case CTID_INT8:
			ival = *(int8_t *) cdata;
			field->type = (ival >= 0) ? MP_UINT : MP_INT;
			field->ival = ival;
			return 0;
		case CTID_INT16:
			ival = *(int16_t *) cdata;
			field->type = (ival >= 0) ? MP_UINT : MP_INT;
			field->ival = ival;
			return 0;
		case CTID_INT32:
			ival = *(int32_t *) cdata;
			field->type = (ival >= 0) ? MP_UINT : MP_INT;
			field->ival = ival;
			return 0;
		case CTID_INT64:
			ival = *(int64_t *) cdata;
			field->type = (ival >= 0) ? MP_UINT : MP_INT;
			field->ival = ival;
			return 0;
		case CTID_UINT8:
			field->type = MP_UINT;
			field->ival = *(uint8_t *) cdata;
			return 0;
		case CTID_UINT16:
			field->type = MP_UINT;
			field->ival = *(uint16_t *) cdata;
			return 0;
		case CTID_UINT32:
			field->type = MP_UINT;
			field->ival = *(uint32_t *) cdata;
			return 0;
		case CTID_UINT64:
			field->type = MP_UINT;
			field->ival = *(uint64_t *) cdata;
			return 0;
		case CTID_FLOAT:
			field->type = MP_FLOAT;
			field->fval = *(float *) cdata;
			CHECK_NUMBER(field->fval);
			return 0;
		case CTID_DOUBLE:
			field->type = MP_DOUBLE;
			field->dval = *(double *) cdata;
			CHECK_NUMBER(field->dval);
			return 0;
		case CTID_P_CVOID:
		case CTID_P_VOID:
			if (*(void **) cdata == NULL) {
				field->type = MP_NIL;
				return 0;
			}
			field->type = MP_EXT;
			field->ext_type = MP_UNKNOWN_EXTENSION;
			return 0;
		default:
			if (ctypeid == CTID_VARBINARY) {
				field->type = MP_BIN;
				field->sval.data = luaT_tovarbinary(
					L, index, &field->sval.len);
				assert(field->sval.data != NULL);
				return 0;
			}
			field->type = MP_EXT;
			if (ctypeid == CTID_DECIMAL) {
				field->ext_type = MP_DECIMAL;
				field->decval = (decimal_t *) cdata;
			} else if (ctypeid == CTID_UUID) {
				field->ext_type = MP_UUID;
				field->uuidval = (struct tt_uuid *) cdata;
			} else if (ctypeid == CTID_CONST_STRUCT_ERROR_REF) {
				field->ext_type = MP_ERROR;
				field->errorval = *(struct error **)cdata;
			} else if (ctypeid == CTID_DATETIME) {
				field->ext_type = MP_DATETIME;
				field->dateval = (struct datetime *)cdata;
			} else if (ctypeid == CTID_INTERVAL) {
				field->ext_type = MP_INTERVAL;
				field->interval = (struct interval *)cdata;
			} else {
				field->ext_type = MP_UNKNOWN_EXTENSION;
			}
		}
		return 0;
	}
	case LUA_TBOOLEAN:
		field->type = MP_BOOL;
		field->bval = lua_toboolean(L, index);
		return 0;
	case LUA_TNIL:
		field->type = MP_NIL;
		return 0;
	case LUA_TSTRING:
		field->sval.data = lua_tolstring(L, index, &size);
		field->sval.len = (uint32_t) size;
		field->type = MP_STR;
		return 0;
	case LUA_TTABLE:
		return lua_field_inspect_table(L, cfg, index, field);
	case LUA_TLIGHTUSERDATA:
	case LUA_TUSERDATA:
		field->sval.data = NULL;
		field->sval.len = 0;
		if (lua_touserdata(L, index) == NULL) {
			field->type = MP_NIL;
			return 0;
		}
		/* Fall through */
	default:
		field->type = MP_EXT;
		field->ext_type = MP_UNKNOWN_EXTENSION;
	}
#undef CHECK_NUMBER
	return 0;
}

int
luaL_convertfield(struct lua_State *L, struct luaL_serializer *cfg, int idx,
		  struct luaL_field *field)
{
	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	assert(field->type == MP_EXT); /* must be called after tofield() */

	if (cfg->encode_load_metatables) {
		int type = lua_type(L, idx);
		if (type == LUA_TCDATA) {
			/*
			 * Don't call __serialize on primitive types
			 * https://github.com/tarantool/tarantool/issues/1226
			 */
			uint32_t ctypeid;
			luaL_tocpointer(L, idx, &ctypeid);
			if (ctypeid > CTID_CTYPEID &&
			    lua_field_inspect_ucdata(L, cfg, idx, field) != 0)
				return -1;
		} else if (type == LUA_TUSERDATA) {
			if (lua_field_inspect_ucdata(L, cfg, idx, field) != 0)
				return -1;
		}
	}

	if (field->type == MP_EXT && field->ext_type == MP_UNKNOWN_EXTENSION &&
	    cfg->encode_use_tostring)
		lua_field_tostring(L, cfg, idx, field);

	if (field->type != MP_EXT || field->ext_type != MP_UNKNOWN_EXTENSION)
		return 0;

	if (cfg->encode_invalid_as_nil) {
		field->type = MP_NIL;
		return 0;
	}

	diag_set(LuajitError,
		 tt_sprintf("unsupported Lua type '%s'",
			    lua_typename(L, lua_type(L, idx))));
	return -1;
}

/* }}} Fill luaL_field */

/* {{{ luaT_reftable */

/**
 * Traversal context for creating a table of references from
 * original objects to serialized ones.
 */
struct reftable_new_ctx {
	/** Serialization options. */
	struct luaL_serializer *cfg;
	/** Index of a references table on the Lua stack. */
	int reftable_index;
	/** Index of a visited objects table on the Lua stack. */
	int visited_index;
};

/**
 * Serialize the object given at top of the Lua stack and all the
 * descendant ones recursively and fill a mapping from the
 * original objects to the resulting ones.
 *
 * The serialization is performed using luaL_checkfield().
 *
 * The function leaves the Lua stack size unchanged.
 */
static void
luaT_reftable_new_impl(struct lua_State *L, struct reftable_new_ctx *ctx)
{
	struct luaL_field field;

	/*
	 * We're not interested in values that can't have
	 * __serialize or __tostring metamethods.
	 */
	if (!lua_istable(L, -1) &&
	    !luaL_iscdata(L, -1) &&
	    !lua_isuserdata(L, -1))
		return;

	/*
	 * Check if the object is already visited.
	 *
	 * Just to don't go into the infinite recursion.
	 */
	if (luaT_hasfield(L, -1, ctx->visited_index))
		return;

	/* Mark the object as visited. */
	lua_pushvalue(L, -1);
	lua_pushboolean(L, true);
	lua_settable(L, ctx->visited_index);

	/*
	 * Check if the object is already saved in the reference
	 * table.
	 */
	if (luaT_hasfield(L, -1, ctx->reftable_index))
		return;

	/*
	 * Copy the original object and serialize it. The
	 * luaL_checkfield() function replaces the value on the
	 * Lua stack with the serialized one (or left it as is).
	 */
	lua_pushvalue(L, -1);
	luaL_checkfield(L, ctx->cfg, -1, &field);

	/*
	 * Save {original object -> serialized object} in the
	 * reference table.
	 */
	if (!lua_rawequal(L, -1, -2)) {
		lua_pushvalue(L, -2); /* original object */
		lua_pushvalue(L, -2); /* serialized object */
		lua_settable(L, ctx->reftable_index);
	}

	/*
	 * Check if the serialized object is already saved in the
	 * reference table.
	 */
	if (luaT_hasfield(L, -1, ctx->reftable_index)) {
		lua_pop(L, 1);
		return;
	}

	/*
	 * Go down into the recursion to analyze the fields if the
	 * serialized object is a table.
	 */
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2)) {
			luaT_reftable_new_impl(L, ctx);
			lua_pop(L, 1);
			luaT_reftable_new_impl(L, ctx);
		}
	}

	/* Pop the serialized value, leave the original one. */
	lua_pop(L, 1);
}

int
luaT_reftable_new(struct lua_State *L, struct luaL_serializer *cfg, int idx)
{
	/*
	 * Fill the traversal context.
	 *
	 * Create a reference table and a visited objects table.
	 */
	struct reftable_new_ctx ctx;
	ctx.cfg = cfg;
	lua_newtable(L);
	ctx.reftable_index = lua_gettop(L);
	lua_newtable(L);
	ctx.visited_index = lua_gettop(L);

	/*
	 * Copy the given object on top of the Lua stack and
	 * traverse all its descendants recursively.
	 *
	 * Fill the reference table for all the met objects that
	 * are changed by the serialization.
	 */
	lua_pushvalue(L, idx);
	luaT_reftable_new_impl(L, &ctx);

	/*
	 * Pop the copy of the given object and the visited
	 * objects table. Leave the reference table on the top.
	 */
	lua_pop(L, 2);

	return 1;
}

void
luaT_reftable_serialize(struct lua_State *L, int reftable_index)
{
	lua_pushvalue(L, -1);
	lua_gettable(L, reftable_index);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
	} else {
		lua_replace(L, -2);
	}
}

/* }}} luaT_reftable */

int
tarantool_lua_serializer_init(struct lua_State *L)
{
	static const struct luaL_Reg serializermeta[] = {
		{"__gc", luaL_serializer_destroy},
		{NULL, NULL},
	};
	luaL_register_type(L, LUAL_SERIALIZER, serializermeta);

	lua_createtable(L, 0, 1);
	lua_pushliteral(L, "map"); /* YAML will use flow mode */
	lua_setfield(L, -2, LUAL_SERIALIZE);
	/* automatically reset hints on table change */
	luaL_loadstring(L, "setmetatable((...), nil); return rawset(...)");
	lua_setfield(L, -2, "__newindex");
	luaL_map_metatable_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_createtable(L, 0, 1);
	lua_pushliteral(L, "seq"); /* YAML will use flow mode */
	lua_setfield(L, -2, LUAL_SERIALIZE);
	/* automatically reset hints on table change */
	luaL_loadstring(L, "setmetatable((...), nil); return rawset(...)");
	lua_setfield(L, -2, "__newindex");
	luaL_array_metatable_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	return 0;
}
