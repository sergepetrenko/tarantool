/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
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
#include "box/lua/tuple.h"
#include "box/lua/tuple_format.h"
#include "box/xrow_update.h"

#include "lua/utils.h" /* luaT_error() */
#include "lua/serializer.h"
#include "lua/msgpack.h" /* luamp_encode_XXX() */
#include "diag.h" /* diag_set() */
#include <small/ibuf.h>
#include <small/region.h>
#include "cord_buf.h"
#include <fiber.h>

#include "box/tuple.h"
#include "box/tuple_convert.h"
#include "box/errcode.h"
#include "json/json.h"
#include "mpstream/mpstream.h"

/** {{{ box.tuple Lua library
 *
 * To avoid extra copying between Lua memory and garbage-collected
 * tuple memory, provide a Lua userdata object 'box.tuple'.  This
 * object refers to a tuple instance in the slab allocator, and
 * allows accessing it using Lua primitives (array subscription,
 * iteration, etc.). When Lua object is garbage-collected,
 * tuple reference counter in the slab allocator is decreased,
 * allowing the tuple to be eventually garbage collected in
 * the slab allocator.
 */

static const char *tuplelib_name = "box.tuple";
static const char *tuple_iteratorlib_name = "box.tuple.iterator";
/*
 * Special serializer for box.tuple.new() to disable storage
 * optimization for excessively sparse arrays as a tuple always
 * must be regular MP_ARRAY.
 */
static struct luaL_serializer tuple_serializer;

extern char tuple_lua[]; /* Lua source */

uint32_t CTID_STRUCT_TUPLE_REF;

/**
 * <luaT_tuple_encode_table>() reference in the Lua registry.
 *
 * Storing of the reference allows to don't create a new GCfunc
 * object each time we call the function in the protected mode.
 * It reduces Lua GC pressure in comparison with calling of
 * <lua_cpcall>() or <lua_pushcfunction>() on each invocation.
 */
static int luaT_tuple_encode_table_ref = LUA_NOREF;
/**
 * <lbox_tuple_gc> reference in the Lua registry.
 *
 * There is no need to create a new GCfunc object each time
 * <luaT_pushtuple> is called, since there are no upvalues passed
 * to <lua_pushcfunction> (i.e. macro to <lua_pushcclosure>).
 * Hence, to reduce Lua GC pressure when tuple object is created
 * for Lua world, the common tuple __gc finalizer can be easily
 * obtained from the Lua registry via this reference.
 */
static int luaT_tuple_gc_ref = LUA_NOREF;

box_tuple_t *
luaT_checktuple(struct lua_State *L, int idx)
{
	struct tuple *tuple = luaT_istuple(L, idx);
	if (tuple == NULL)  {
		luaL_error(L, "Invalid argument #%d (box.tuple expected, got %s)",
		   idx, lua_typename(L, lua_type(L, idx)));
	}

	return tuple;
}

box_tuple_t *
luaT_istuple(struct lua_State *L, int narg)
{
	assert(CTID_STRUCT_TUPLE_REF != 0);
	uint32_t ctypeid;
	void *data;

	if (lua_type(L, narg) != LUA_TCDATA)
		return NULL;

	data = luaL_checkcdata(L, narg, &ctypeid);
	if (ctypeid != CTID_STRUCT_TUPLE_REF)
		return NULL;

	return *(struct tuple **) data;
}

/* {{{ Encode a Lua table as an MsgPack array */

/*
 * A lot of functions are there, however the task per se looks
 * simple. Reasons are the following.
 *
 * 1. box.tuple.new() supports two parameters conventions.
 *    <luaT_tuple_encode_values>() implements the old API.
 * 2. Serializer from Lua to MsgPack may raise a Lua error,
 *    so it should be run under pcall. The dangerous code is
 *    encapsulated into <luaT_tuple_encode_table>().
 * 3. In particular <mpstream_init>() may raise an error in case
 *    of OOM, so it also is run under pcall.
 * 4. box.tuple.new() and <luaT_tuple_new>() use shared Lua ibuf
 *    under the hood (because there is no strong reason to change
 *    it), while <luaT_tuple_encode>() uses the box region
 *    (because it is usual for the module API).
 */

typedef void luaT_mpstream_init_f(struct mpstream *stream, struct lua_State *L,
				  void *buffer);

static void
luaT_mpstream_init_lua_ibuf(struct mpstream *stream, struct lua_State *L,
			    void *buffer)
{
	mpstream_init(stream, (struct ibuf *)buffer, ibuf_reserve_cb,
		      ibuf_alloc_cb, luamp_error, L);
}

static void
luaT_mpstream_init_box_region(struct mpstream *stream, struct lua_State *L,
			      void *buffer)
{
	mpstream_init(stream, (struct region *)buffer, region_reserve_cb,
		      region_alloc_cb, luamp_error, L);
}

/** Helper to pass parameters into luaT_tuple_encode_table via the Lua stack. */
struct luaT_tuple_encode_ctx {
	luaT_mpstream_init_f *mpstream_init_f;
	void *buffer;
};

/**
 * Encode a Lua table or a tuple as MsgPack.
 *
 * Raise a Lua error when encoding fails.
 *
 * It is a kind of critical section to be run under luaT_call().
 */
static int
luaT_tuple_encode_table(struct lua_State *L)
{
	struct mpstream stream;
	const struct luaT_tuple_encode_ctx *ctx = lua_topointer(L, 1);
	ctx->mpstream_init_f(&stream, L, ctx->buffer);
	if (luamp_encode_tuple(L, &tuple_serializer, &stream, 2) != 0)
		luaT_error(L);
	mpstream_flush(&stream);
	return 0;
}

/**
 * Encode a Lua table / tuple to given mpstream.
 */
static int
luaT_tuple_encode_on_mpstream(struct lua_State *L, int idx,
			      luaT_mpstream_init_f *luaT_mpstream_init_f,
			      void *buffer)
{
	assert(idx != 0);
	if (!lua_istable(L, idx) && !luaT_istuple(L, idx)) {
		diag_set(IllegalParams, "A tuple or a table expected, got %s",
			 lua_typename(L, lua_type(L, idx)));
		return -1;
	}

	/* To restore before leaving the function. */
	int top = lua_gettop(L);

	/*
	 * An absolute index doesn't need to be recalculated after
	 * the stack size change.
	 */
	if (idx < 0)
		idx = top + idx + 1;

	assert(luaT_tuple_encode_table_ref != LUA_NOREF);
	lua_rawgeti(L, LUA_REGISTRYINDEX, luaT_tuple_encode_table_ref);
	assert(lua_isfunction(L, -1));

	struct luaT_tuple_encode_ctx ctx = {
		.mpstream_init_f = luaT_mpstream_init_f,
		.buffer = buffer,
	};
	lua_pushlightuserdata(L, &ctx);
	lua_pushvalue(L, idx);

	int rc = luaT_call(L, 2, 0);
	lua_settop(L, top);
	return rc;
}

/**
 * Encode a Lua table / tuple to Lua shared ibuf.
 */
static char *
luaT_tuple_encode_on_lua_ibuf(struct lua_State *L, int idx,
			      size_t *tuple_len_ptr, struct ibuf *buf)
{
	if (luaT_tuple_encode_on_mpstream(L, idx,
					  luaT_mpstream_init_lua_ibuf,
					  buf) != 0)
		return NULL;
	if (tuple_len_ptr != NULL)
		*tuple_len_ptr = ibuf_used(buf);
	return buf->buf;
}

/**
 * Encode a Lua table / tuple to box region.
 */
char *
luaT_tuple_encode(struct lua_State *L, int idx, size_t *tuple_len_ptr)
{
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	if (luaT_tuple_encode_on_mpstream(L, idx,
					  luaT_mpstream_init_box_region,
					  region) != 0) {
		region_truncate(region, region_svp);
		return NULL;
	}
	size_t tuple_len = region_used(region) - region_svp;
	if (tuple_len_ptr != NULL)
		*tuple_len_ptr = tuple_len;
	char *tuple_data = region_join(region, tuple_len);
	if (tuple_data == NULL) {
		diag_set(OutOfMemory, tuple_len, "region", "tuple data");
		region_truncate(region, region_svp);
		return NULL;
	}
	return tuple_data;
}

/* }}} Encode a Lua table as an MsgPack array */

box_tuple_t *
luaT_tuple_new(struct lua_State *L, int idx, box_tuple_format_t *format)
{
	struct ibuf *ibuf = cord_ibuf_take();
	size_t tuple_len;
	box_tuple_t *tuple;
	char *tuple_data = luaT_tuple_encode_on_lua_ibuf(L, idx, &tuple_len,
							 ibuf);
	if (tuple_data == NULL) {
		tuple = NULL;
	} else {
		tuple = box_tuple_new(format, tuple_data,
				      tuple_data + tuple_len);
	}
	cord_ibuf_drop(ibuf);
	return tuple;
}

static int
lbox_tuple_new(lua_State *L)
{
	int argc = lua_gettop(L);
	/*
	 * Use the new parameters format:
	 * box.tuple.new({tuple_field1, tuple_field2, tuple_field3}, [options]).
	 */
	struct tuple_format *format;
	if (argc == 2)
		format = luaT_check_tuple_format(L, -1);
	else
		format = box_tuple_format_default();
	struct tuple *tuple = luaT_tuple_new(L, 1, format);
	if (tuple == NULL)
		return luaT_error(L);
	/* box_tuple_new() doesn't leak on exception, see public API doc */
	luaT_pushtuple(L, tuple);
	return 1;
}

static int
lbox_tuple_gc(struct lua_State *L)
{
	struct tuple *tuple = luaT_checktuple(L, 1);
	box_tuple_unref(tuple);
	return 0;
}

static int
lbox_tuple_slice_wrapper(struct lua_State *L)
{
	box_tuple_iterator_t *it = (box_tuple_iterator_t *)
		lua_topointer(L, 1);
	uint32_t start = lua_tonumber(L, 2);
	uint32_t end = lua_tonumber(L, 3);
	assert(end >= start);
	const char *field;

	uint32_t field_no = start;
	field = box_tuple_seek(it, start);
	while (field && field_no < end) {
		luamp_decode(L, luaL_msgpack_default, &field);
		++field_no;
		field = box_tuple_next(it);
	}
	assert(field_no == end);
	return end - start;
}

static int
lbox_tuple_slice(struct lua_State *L)
{
	struct tuple *tuple = luaT_checktuple(L, 1);
	int argc = lua_gettop(L) - 1;
	uint32_t start, end;
	int32_t offset;

	/*
	 * Prepare the range. The second argument is optional.
	 * If the end is beyond tuple size, adjust it.
	 * If no arguments, or start > end, return an error.
	 */
	if (argc == 0 || argc > 2)
		luaL_error(L, "tuple.slice(): bad arguments");

	int32_t field_count = box_tuple_field_count(tuple);
	offset = lua_tonumber(L, 2);
	if (offset >= 0 && offset < field_count) {
		start = offset;
	} else if (offset < 0 && -offset <= field_count) {
		start = offset + field_count;
	} else {
		return luaL_error(L, "tuple.slice(): start >= field count");
	}

	if (argc == 2) {
		offset = lua_tonumber(L, 3);
		if (offset > 0 && offset <= field_count) {
			end = offset;
		} else if (offset < 0 && -offset < field_count) {
			end = offset + field_count;
		} else {
			return luaL_error(L, "tuple.slice(): end > field count");
		}
	} else {
		end = field_count;
	}
	if (end <= start)
		return luaL_error(L, "tuple.slice(): start must be less than end");

	box_tuple_iterator_t *it = box_tuple_iterator(tuple);
	lua_pushcfunction(L, lbox_tuple_slice_wrapper);
	lua_pushlightuserdata(L, it);
	lua_pushinteger(L, start);
	lua_pushinteger(L, end);
	int rc = luaT_call(L, 3, end - start);
	box_tuple_iterator_free(it);
	if (rc != 0)
		return luaT_error(L);
	return end - start;
}

int
luamp_convert_key(struct lua_State *L, struct luaL_serializer *cfg,
		  struct mpstream *stream, int index)
{
	/* Performs keyfy() logic */

	struct tuple *tuple = luaT_istuple(L, index);
	if (tuple != NULL) {
		tuple_to_mpstream(tuple, stream);
		return 0;
	}

	size_t data_len;
	const char *data = luamp_get(L, index, &data_len);
	if (data != NULL) {
		mpstream_memcpy(stream, data, data_len);
		return 0;
	}

	struct luaL_field field;
	if (luaL_tofield(L, cfg, index, &field) < 0)
		return -1;
	int rc = -1;
	if (field.type == MP_ARRAY) {
		lua_pushvalue(L, index);
		rc = luamp_encode_r(L, cfg, stream, &field, 0);
		lua_pop(L, 1);
	} else if (field.type == MP_NIL) {
		mpstream_encode_array(stream, 0);
		rc = 0;
	} else {
		mpstream_encode_array(stream, 1);
		lua_pushvalue(L, index);
		rc = luamp_encode_r(L, cfg, stream, &field, 0);
		lua_pop(L, 1);
	}
	return rc;
}

int
luamp_encode_tuple_with_ctx(struct lua_State *L, struct luaL_serializer *cfg,
			    struct mpstream *stream, int index,
			    struct mp_ctx *ctx)
{
	struct tuple *tuple = luaT_istuple(L, index);
	if (tuple != NULL) {
		tuple_to_mpstream(tuple, stream);
		return 0;
	}

	/*
	 * This snippet handles a special when a box tuple is sent over IPROTO
	 * as MP_TUPLE and is decoded by net.box using the `return_raw` option,
	 * which return a MsgPack object. This case is semantically equivalent
	 * to the case above where the MP_TUPLE should have been decoded as a
	 * box tuple. While we expect the encoded MsgPack to be an MP_ARRAY, the
	 * Lua MsgPack encoder below encodes MsgPack objects by simply copying
	 * their contents to the MsgPack stream, so we would get MP_TUPLE as the
	 * returned type.
	 *
	 * To overcome this limitation, we convert the top-level MP_TUPLE to a
	 * MsgPack array by skipping the extension header and format identifier
	 * and copying the tuple data to the MsgPack stream.
	 */
	size_t data_len;
	const char *data = luamp_get(L, index, &data_len);
	if (data != NULL) {
		if (mp_typeof(*data) == MP_EXT) {
			const char *tuple_data = data;
			int8_t ext_type;
			uint32_t tuple_data_len =
				mp_decode_extl(&tuple_data, &ext_type);
			if (ext_type == MP_TUPLE) {
				/* Skip the tuple format identifier. */
				assert(mp_typeof(*tuple_data) == MP_UINT);
				uint64_t format_id =
					mp_decode_uint(&tuple_data);
				tuple_data_len -= mp_sizeof_uint(format_id);
				mpstream_memcpy(stream, tuple_data,
						tuple_data_len);
				return 0;
			}
		}
	}

	enum mp_type type;
	if (luamp_encode_with_ctx(L, cfg, stream, index, ctx, &type) != 0)
		return -1;
	if (type != MP_ARRAY) {
		diag_set(ClientError, ER_TUPLE_NOT_ARRAY);
		return -1;
	}

	return 0;
}

void
tuple_to_mpstream(struct tuple *tuple, struct mpstream *stream)
{
	size_t bsize = box_tuple_bsize(tuple);
	char *ptr = mpstream_reserve(stream, bsize);
	box_tuple_to_buf(tuple, ptr, bsize);
	mpstream_advance(stream, bsize);
}

/**
 * Convert a tuple into lua table. Named fields are stored as
 * {name = value} pairs. Not named fields are stored as
 * {1-based_index_in_tuple = value}.
 */
static int
lbox_tuple_to_map(struct lua_State *L)
{
	int argc = lua_gettop(L);
	if (argc < 1 || argc > 2)
		goto error;
	bool names_only = false;
	if (argc == 2) {
		if (!lua_istable(L, 2))
			goto error;
		lua_getfield(L, 2, "names_only");
		if (!lua_isboolean(L, -1) && !lua_isnil(L, -1))
			goto error;
		names_only = lua_toboolean(L, -1);
	}

	struct tuple *tuple = luaT_checktuple(L, 1);
	struct tuple_format *format = tuple_format(tuple);
	const char *pos = tuple_data(tuple);
	int field_count = (int)mp_decode_array(&pos);
	int n_named = format->dict->name_count;
	lua_createtable(L, field_count, n_named);
	int named_and_presented = MIN(field_count, n_named);
	for (int i = 0; i < named_and_presented; ++i) {
		/* Access by name. */
		const char *name = format->dict->names[i];
		lua_pushstring(L, name);
		luamp_decode(L, luaL_msgpack_default, &pos);
		lua_rawset(L, -3);
		if (names_only)
			continue;
		/*
		 * Access the same field by an index. There is no
		 * copy for tables - lua optimizes it and uses
		 * references.
		 */
		lua_pushstring(L, name);
		lua_rawget(L, -2);
		lua_rawseti(L, -2, i + TUPLE_INDEX_BASE);
	}
	if (names_only)
		return 1;
	/* Access for not named fields by index. */
	for (int i = n_named; i < field_count; ++i) {
		luamp_decode(L, luaL_msgpack_default, &pos);
		lua_rawseti(L, -2, i + TUPLE_INDEX_BASE);
	}
	return 1;
error:
	luaL_error(L, "Usage: tuple:tomap(opts)");
	return 1;
}

/**
 * Tuple transforming function.
 *
 * Remove the fields designated by 'offset' and 'len' from an tuple,
 * and replace them with the elements of supplied data fields,
 * if any.
 *
 * Function returns newly allocated tuple.
 * It does not change any parent tuple data.
 */
static int
lbox_tuple_transform(struct lua_State *L)
{
	struct tuple *tuple = luaT_checktuple(L, 1);
	int argc = lua_gettop(L);
	if (argc < 3)
		luaL_error(L, "tuple.transform(): bad arguments");
	lua_Integer offset = lua_tointeger(L, 2);  /* Can be negative and can be > INT_MAX */
	lua_Integer len = lua_tointeger(L, 3);

	lua_Integer field_count = box_tuple_field_count(tuple);
	/* validate offset and len */
	if (offset == 0) {
		luaL_error(L, "tuple.transform(): offset is out of bound");
	} else if (offset < 0) {
		if (-offset > field_count)
			luaL_error(L, "tuple.transform(): offset is out of bound");
		offset += field_count + 1;
	} else if (offset > field_count) {
		offset = field_count + 1;
	}
	if (len < 0)
		luaL_error(L, "tuple.transform(): len is negative");
	if (len > field_count + 1 - offset)
		len = field_count + 1 - offset;

	assert(offset + len <= field_count + 1);

	/*
	 * Calculate the number of operations and length of UPDATE expression
	 */
	uint32_t op_cnt = 0;
	if (offset < field_count + 1 && len > 0)
		op_cnt++;
	if (argc > 3)
		op_cnt += argc - 3;

	if (op_cnt == 0) {
		luaT_pushtuple(L, tuple);
		return 1;
	}

	struct tuple *new_tuple = NULL;
	struct ibuf *buf = cord_ibuf_take();
	struct mpstream stream;
	mpstream_init(&stream, buf, ibuf_reserve_cb, ibuf_alloc_cb,
		      luamp_error, L);

	/*
	 * Prepare UPDATE expression
	 */
	mpstream_encode_array(&stream, op_cnt);
	if (len > 0) {
		mpstream_encode_array(&stream, 3);
		mpstream_encode_str(&stream, "#");
		mpstream_encode_uint(&stream, offset);
		mpstream_encode_uint(&stream, len);
	}

	for (int i = argc ; i > 3; i--) {
		mpstream_encode_array(&stream, 3);
		mpstream_encode_str(&stream, "!");
		mpstream_encode_uint(&stream, offset);
		if (luamp_encode(L, luaL_msgpack_default, &stream, i) != 0)
			goto cleanup;
	}
	mpstream_flush(&stream);

	uint32_t new_size = 0, bsize;
	const char *old_data = tuple_data_range(tuple, &bsize);
	struct region *region = &fiber()->gc;
	size_t used = region_used(region);
	struct tuple_format *format = tuple_format(tuple);
	/*
	 * Can't use box_tuple_update() since transform must reset
	 * the tuple format to default. The new tuple most likely
	 * won't coerce into the original space format, so we have
	 * to use the default one with no restrictions on field
	 * count or types.
	 */
	const char *new_data =
		xrow_update_execute(buf->buf, buf->buf + ibuf_used(buf),
				    old_data, old_data + bsize, format,
				    &new_size, 1, NULL);
	if (new_data != NULL)
		new_tuple = tuple_new(box_tuple_format_default(),
				      new_data, new_data + new_size);
	region_truncate(region, used);
cleanup:
	cord_ibuf_put(buf);
	if (new_tuple == NULL)
		luaT_error(L);

	luaT_pushtuple(L, new_tuple);
	return 1;
}

static int
lbox_tuple_to_string(struct lua_State *L)
{
	struct tuple *tuple = luaT_checktuple(L, 1);
	size_t used = region_used(&fiber()->gc);
	char *res = tuple_to_yaml(tuple);
	if (res == NULL) {
		region_truncate(&fiber()->gc, used);
		return luaT_error(L);
	}
	lua_pushstring(L, res);
	region_truncate(&fiber()->gc, used);
	return 1;
}

void
luaT_pushtuple(struct lua_State *L, box_tuple_t *tuple)
{
	assert(CTID_STRUCT_TUPLE_REF != 0);
	struct tuple **ptr = (struct tuple **)
		luaL_pushcdata(L, CTID_STRUCT_TUPLE_REF);
	*ptr = tuple;
	/* The order is important - first reference tuple, next set gc */
	box_tuple_ref(tuple);
	assert(luaT_tuple_gc_ref != LUA_NOREF);
	lua_rawgeti(L, LUA_REGISTRYINDEX, luaT_tuple_gc_ref);
	luaL_setcdatagc(L, -2);
}

/**
 * Push to Lua stack a table with the information about a tuple, located on top
 * of the stack.
 */
static int
lbox_tuple_info(lua_State *L)
{
	int argc = lua_gettop(L);
	if (argc != 1)
		luaL_error(L, "Usage: tuple:info()");

	struct tuple *tuple = luaT_checktuple(L, 1);
	struct tuple_info info;
	tuple_info(tuple, &info);

	lua_newtable(L);

	lua_pushnumber(L, info.data_size);
	lua_setfield(L, -2, "data_size");

	lua_pushnumber(L, info.header_size);
	lua_setfield(L, -2, "header_size");

	lua_pushnumber(L, info.field_map_size);
	lua_setfield(L, -2, "field_map_size");

	lua_pushnumber(L, info.waste_size);
	lua_setfield(L, -2, "waste_size");

	lua_pushstring(L, tuple_arena_type_strs[info.arena_type]);
	lua_setfield(L, -2, "arena");

	return 1;
}

/**
 * Push to Lua stack an array with the information about a format of a tuple.
 * Elements of the array are maps
 * {'name' = 'field_name', 'type' = 'field_type'}.
 */
static int
lbox_tuple_get_format(lua_State *L)
{
	int argc = lua_gettop(L);
	if (argc != 1)
		luaL_error(L, "Usage: tuple:format()");

	struct tuple *tuple = luaT_checktuple(L, 1);
	struct tuple_format *format = tuple_format(tuple);
	return box_tuple_format_serialize_impl(L, format);
}

static const struct luaL_Reg lbox_tuple_meta[] = {
	{"__gc", lbox_tuple_gc},
	{"tostring", lbox_tuple_to_string},
	{"slice", lbox_tuple_slice},
	{"transform", lbox_tuple_transform},
	{"tuple_to_map", lbox_tuple_to_map},
	{"new", lbox_tuple_new},
	{"info", lbox_tuple_info},
	{"tuple_get_format", lbox_tuple_get_format},
	{NULL, NULL}
};

static const struct luaL_Reg lbox_tuple_iterator_meta[] = {
	{NULL, NULL}
};

/* }}} */

static inline void
tuple_serializer_update_options(void)
{
	luaL_serializer_copy_options(&tuple_serializer, luaL_msgpack_default);
	tuple_serializer.encode_sparse_ratio = 0;
}

static int
on_msgpack_serializer_update(struct trigger *trigger, void *event)
{
	(void) trigger;
	(void) event;
	tuple_serializer_update_options();
	return 0;
}

void
box_lua_tuple_init(struct lua_State *L)
{
	/* export C functions to Lua */
	luaL_findtable(L, LUA_GLOBALSINDEX, "box.internal", 1);
	luaL_newmetatable(L, tuplelib_name);
	luaL_setfuncs(L, lbox_tuple_meta, 0);
	lua_setfield(L, -2, "tuple");
	lua_pop(L, 1); /* box.internal */
	luaL_register_type(L, tuple_iteratorlib_name,
			   lbox_tuple_iterator_meta);

	tuple_serializer_update_options();
	trigger_create(&tuple_serializer.update_trigger,
		       on_msgpack_serializer_update, NULL, NULL);
	trigger_add(&luaL_msgpack_default->on_update,
		    &tuple_serializer.update_trigger);

	/* Get CTypeID for `struct tuple' */
	int rc = luaL_cdef(L, "struct tuple;");
	assert(rc == 0);
	(void) rc;
	CTID_STRUCT_TUPLE_REF = luaL_ctypeid(L, "struct tuple &");
	assert(CTID_STRUCT_TUPLE_REF != 0);

	lua_pushcfunction(L, luaT_tuple_encode_table);
	luaT_tuple_encode_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_pushcfunction(L, lbox_tuple_gc);
	luaT_tuple_gc_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}
