#include "memory.h"
#include "fiber.h"
#include "tuple.h"
#include "memtx_engine.h"
#include <allocator.h>

#include <benchmark/benchmark.h>
#include <random>

static const size_t NUM_TEST_TUPLES = 4096;

struct random_generator {
	random_generator() : generator(dev()), get_i(0)
	{
	}

	uint64_t get()
	{
		int i = get_i;
		get_i = (get_i + 1) % 4;
		/*
		 * The different int ranges don't grow linearly. Always using
		 * the [0, UINT64_MAX] range mostly returns numbers >
		 * UINT32_MAX, which isn't fair. To make the type distribution
		 * more even the types are selected in round-robin. And only
		 * inside each type the selection is random.
		 */
		switch (i) {
		case 0:
			return std::uniform_int_distribution<uint8_t>(
				0, UINT8_MAX)(generator);
		case 1:
			return std::uniform_int_distribution<uint16_t>(
				0, UINT16_MAX)(generator);
		case 2:
			return std::uniform_int_distribution<uint32_t>(
				0, UINT32_MAX)(generator);
		case 3:
			return std::uniform_int_distribution<uint64_t>(
				0, UINT64_MAX)(generator);
		default:
			unreachable();
			return 0;
		}
	}

	std::random_device dev;
	std::mt19937 generator;
	int get_i;
};

static random_generator random_gen;

enum data_format {
	/** 5 fields (UINT, STR, NIL, UINT, UINT) + 0-95 optional UINTs. */
	FORMAT_BASIC,
	/** 1000 UINT fields, 990 of them are NIL. */
	FORMAT_SPARSE,
};

static uint32_t
test_field_name_hash(const char *str, uint32_t len)
{
	return str[0] + len;
}

/** Class that creates and destroys memtx engine. */
class MemtxEngine {
public:
	static MemtxEngine &instance()
	{
		static MemtxEngine instance;
		return instance;
	}
	struct memtx_engine *engine() { return &memtx; }
private:
	MemtxEngine()
	{
		memory_init();
		fiber_init(fiber_c_invoke);
		region_alloc(&fiber()->gc, 4);
		tuple_init(test_field_name_hash);

		memset(&memtx, 0, sizeof(memtx));

		quota_init(&memtx.quota, QUOTA_MAX);

		int rc;
		rc = slab_arena_create(&memtx.arena, &memtx.quota,
				       16 * 1024 * 1024, 16 * 1024 * 1024,
				       SLAB_ARENA_PRIVATE);
		if (rc != 0)
			abort();

		slab_cache_create(&memtx.slab_cache, &memtx.arena);

		float actual_alloc_factor;
		allocator_settings alloc_settings;
		allocator_settings_init(&alloc_settings, &memtx.slab_cache,
					16, 8, 1.1, &actual_alloc_factor,
					&memtx.quota);
		SmallAlloc::create(&alloc_settings);
		memtx_set_tuple_format_vtab("small");

		memtx.max_tuple_size = 1024 * 1024;
	}
	~MemtxEngine()
	{
		tuple_free();
		SmallAlloc::destroy();
		slab_cache_destroy(&memtx.slab_cache);
		tuple_arena_destroy(&memtx.arena);
		fiber_free();
		memory_free();
	}

	struct memtx_engine memtx;
};

/** Class that creates and destroys tuple format. */
template<data_format F>
class TupleFormat;

template<>
class TupleFormat<FORMAT_BASIC> {
public:
	static struct tuple_format *make(struct key_def *kd)
	{
		struct memtx_engine *memtx = MemtxEngine::instance().engine();
		struct tuple_format *fmt = simple_tuple_format_new(
			&memtx_tuple_format_vtab, memtx, &kd, 1);
		if (fmt == NULL)
			abort();
		tuple_format_ref(fmt);
		return fmt;
	}
};

template<>
class TupleFormat<FORMAT_SPARSE> {
public:
	static struct tuple_format *make(struct key_def *kd)
	{
		struct memtx_engine *memtx = MemtxEngine::instance().engine();

		std::vector<char[sizeof("f999")]> names(FIELD_COUNT);
		std::vector<struct field_def> fields(FIELD_COUNT);
		for (size_t i = 0; i < FIELD_COUNT; i++) {
			sprintf(names[i], "f%03zu", i);
			fields[i] = field_def_default;
			fields[i].name = names[i];
			fields[i].type = FIELD_TYPE_UNSIGNED;
			fields[i].is_nullable = true;
			fields[i].nullable_action = ON_CONFLICT_ACTION_NONE;
		}
		struct tuple_dictionary *dict =
			tuple_dictionary_new(fields.data(), FIELD_COUNT);
		if (dict == NULL)
			abort();
		struct tuple_format *fmt = tuple_format_new(
			&memtx_tuple_format_vtab, memtx, &kd, 1, fields.data(),
			FIELD_COUNT, FIELD_COUNT, dict, false, false, NULL, 0,
			NULL, 0);
		if (fmt == NULL)
			abort();
		tuple_format_ref(fmt);
		tuple_dictionary_unref(dict);
		return fmt;
	}

	static const size_t FIELD_COUNT = 1000;
};

// Generator of random msgpack array.
template<data_format F>
class MpData;

template<>
class MpData<FORMAT_BASIC> {
public:
	const char *begin() const { return data; }
	const char *end() const { return data_end; }
	MpData()
	{
		uint64_t r1 = random_gen.get();
		uint64_t r2 = random_gen.get();
		uint64_t r3 = random_gen.get();
		const size_t common_size = 12 + mp_sizeof_uint(r1) +
			mp_sizeof_uint(r2) + mp_sizeof_uint(r3);
		size_t add_bytes = rand() % (MAX_TUPLE_DATA_SIZE - common_size);
		size_t add_nums = add_bytes / 5;

		data_end = data;
		data_end = mp_encode_array(data_end, 5 + add_nums);
		data_end = mp_encode_uint(data_end, r1);
		data_end = mp_encode_str(data_end, "hello", 5);
		data_end = mp_encode_nil(data_end);
		data_end = mp_encode_uint(data_end, r2);
		data_end = mp_encode_uint(data_end, r3);
		if (data_end - data > common_size)
			abort();
		for (size_t i = 0; i < add_nums; i++)
			data_end = mp_encode_uint(data_end, 0xFFFFFF);
		if (data_end - data > MAX_TUPLE_DATA_SIZE)
			abort();
	}
private:
	static const size_t MAX_TUPLE_DATA_SIZE = 512;
	char data[MAX_TUPLE_DATA_SIZE];
	char *data_end;
};

template<>
class MpData<FORMAT_SPARSE> {
public:
	const char *begin() const { return data; }
	const char *end() const { return data_end; }
	MpData()
	{
		size_t field_count = TupleFormat<FORMAT_SPARSE>::FIELD_COUNT;
		std::vector<bool> is_non_null(field_count);
		for (size_t i = 0; i < FIELD_COUNT_NON_NULL; i++)
			is_non_null[rand() % field_count] = true;

		data_end = data;
		data_end = mp_encode_array(data_end, field_count);
		for (size_t i = 0; i < field_count; i++) {
			if (is_non_null[i]) {
				data_end = mp_encode_uint(data_end,
							  random_gen.get());
			} else {
				data_end = mp_encode_nil(data_end);
			}
		}
		if (data_end - data > MAX_TUPLE_DATA_SIZE)
			abort();
	}
private:
	static const size_t FIELD_COUNT_NON_NULL = 10;
	static const size_t MAX_TUPLE_DATA_SIZE = 2048;
	char data[MAX_TUPLE_DATA_SIZE];
	char *data_end;
};

// Generator of set of random msgpack arrays.
template<data_format F>
class MpDataSet {
public:
	MpDataSet() : data(NUM_TEST_TUPLES) {}
	const MpData<F> &operator[](size_t i) const { return data[i]; }
private:
	std::vector<MpData<F>> data;
};

// Generator of a set of random tuples.
template<data_format F>
class TestTuples {
public:
	TestTuples(struct tuple_format *format)
	{
		MpDataSet<F> dataset;
		for (size_t i = 0; i < NUM_TEST_TUPLES; i++) {
			data[i] = box_tuple_new(format,
						dataset[i].begin(),
						dataset[i].end());
			tuple_ref(data[i]);
		}
	}
	~TestTuples()
	{
		for (size_t i = 0; i < NUM_TEST_TUPLES; i++)
			tuple_unref(data[i]);

	}
	struct tuple *operator[](size_t i) { return data[i]; }

private:
	struct tuple *data[NUM_TEST_TUPLES];
};

template<data_format F>
struct BenchDataSimple {
	BenchDataSimple()
		: kd([]() {
			struct key_part_def kdp = key_part_def_default;
			kdp.fieldno = 4;
			kdp.type = FIELD_TYPE_UNSIGNED;
			return key_def_new(&kdp, 1, 0);
		} ())
		, format(TupleFormat<F>::make(kd))
		, tuples(format)
	{
	}

	~BenchDataSimple()
	{
		tuple_format_unref(format);
		key_def_delete(kd);
	}

	struct key_def *kd;
	struct tuple_format *format;
	TestTuples<F> tuples;
};

// box_tuple_new benchmark.
template<data_format F>
static void
bench_tuple_new(benchmark::State& state)
{
	struct key_part_def kdp = key_part_def_default;
	kdp.fieldno = 4;
	kdp.type = FIELD_TYPE_UNSIGNED;
	kdp.is_nullable = true;
	kdp.nullable_action = ON_CONFLICT_ACTION_NONE;
	struct key_def *kd = key_def_new(&kdp, 1, 0);
	size_t total_count = 0;

	struct tuple_format *format = TupleFormat<F>::make(kd);
	MpDataSet<F> dataset;
	struct tuple *tuples[NUM_TEST_TUPLES];
	size_t i = 0;

	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			state.PauseTiming();
			for (size_t k = 0; k < NUM_TEST_TUPLES; k++)
				tuple_unref(tuples[k]);
			i = 0;
			state.ResumeTiming();
		}
		tuples[i] = box_tuple_new(format,
					  dataset[i].begin(),
					  dataset[i].end());
		tuple_ref(tuples[i]);
		++i;
	}
	total_count += i;
	state.SetItemsProcessed(total_count);

	for (size_t k = 0; k < i; k++)
		tuple_unref(tuples[k]);
	tuple_format_unref(format);
	key_def_delete(kd);
}

BENCHMARK_TEMPLATE(bench_tuple_new, FORMAT_BASIC);
BENCHMARK_TEMPLATE(bench_tuple_new, FORMAT_SPARSE);

// memtx_tuple_delete benchmark.
template<data_format F>
static void
bench_tuple_delete(benchmark::State& state)
{
	struct key_part_def kdp = key_part_def_default;
	kdp.fieldno = 4;
	kdp.type = FIELD_TYPE_UNSIGNED;
	struct key_def *kd = key_def_new(&kdp, 1, 0);
	size_t total_count = 0;

	struct tuple_format *format = TupleFormat<F>::make(kd);
	MpDataSet<F> dataset;
	struct tuple *tuples[NUM_TEST_TUPLES];

	size_t i = NUM_TEST_TUPLES;
	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			state.PauseTiming();
			for (size_t k = 0; k < NUM_TEST_TUPLES; k++) {
				tuples[k] = box_tuple_new(format,
							  dataset[k].begin(),
							  dataset[k].end());
				tuple_ref(tuples[k]);
			}
			i = 0;
			state.ResumeTiming();
		}
		tuple_unref(tuples[i++]);
	}
	total_count += i;
	state.SetItemsProcessed(total_count);

	for (size_t k = i; k < NUM_TEST_TUPLES; k++)
		tuple_unref(tuples[k]);
	tuple_format_unref(format);
	key_def_delete(kd);
}

BENCHMARK_TEMPLATE(bench_tuple_delete, FORMAT_BASIC);

template<data_format F>
static void
bench_tuple_ref_unref_low(benchmark::State& state)
{
	BenchDataSimple<F> data;
	size_t total_count = 0;
	const size_t NUM_REFS = 32;
	for (auto _ : state) {
		for (size_t k = 0; k < NUM_REFS; k++)
			for (size_t i = 0; i < NUM_TEST_TUPLES; i++)
				tuple_ref(data.tuples[i]);
		for (size_t k = 0; k < NUM_REFS; k++)
			for (size_t i = 0; i < NUM_TEST_TUPLES; i++)
				tuple_unref(data.tuples[i]);
		total_count += NUM_REFS * NUM_TEST_TUPLES;
	}
	state.SetItemsProcessed(total_count);
}

BENCHMARK_TEMPLATE(bench_tuple_ref_unref_low, FORMAT_BASIC);

template<data_format F>
static void
bench_tuple_ref_unref_high(benchmark::State& state)
{
	BenchDataSimple<F> data;
	size_t total_count = 0;
	const size_t NUM_REFS = 1024;
	for (auto _ : state) {
		for (size_t k = 0; k < NUM_REFS; k++)
			for (size_t i = 0; i < NUM_TEST_TUPLES; i++)
				tuple_ref(data.tuples[i]);
		for (size_t k = 0; k < NUM_REFS; k++)
			for (size_t i = 0; i < NUM_TEST_TUPLES; i++)
				tuple_unref(data.tuples[i]);
		total_count += NUM_REFS * NUM_TEST_TUPLES;
	}
	state.SetItemsProcessed(total_count);
}

BENCHMARK_TEMPLATE(bench_tuple_ref_unref_high, FORMAT_BASIC);

// struct tuple member access benchmark.
template<data_format F>
static void
tuple_access_members(benchmark::State& state)
{
	BenchDataSimple<F> data;
	size_t i = 0;
	size_t total_count = 0;
	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			i = 0;
		}
		struct tuple *t = data.tuples[i++];
		// Previously tuple had is_dirty bit field, which was later
		// replaced with tuple flags. So to avoid changing test
		// semantics we check now if tuple has corresponding flag.
		benchmark::DoNotOptimize(tuple_has_flag(t, TUPLE_IS_DIRTY));
		benchmark::DoNotOptimize(uint16_t(t->format_id));
	}
	total_count += i;
	state.SetItemsProcessed(total_count);
}

BENCHMARK_TEMPLATE(tuple_access_members, FORMAT_BASIC);

// tuple_data benchmark.
template<data_format F>
static void
tuple_access_data(benchmark::State& state)
{
	BenchDataSimple<F> data;
	size_t i = 0;
	size_t total_count = 0;
	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			i = 0;
		}
		struct tuple *t = data.tuples[i++];
		benchmark::DoNotOptimize(*tuple_data(t));
	}
	total_count += i;
	state.SetItemsProcessed(total_count);
}

BENCHMARK_TEMPLATE(tuple_access_data, FORMAT_BASIC);

// tuple_data_range benchmark.
template<data_format F>
static void
tuple_access_data_range(benchmark::State& state)
{
	BenchDataSimple<F> data;
	size_t i = 0;
	size_t total_count = 0;
	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			i = 0;
		}
		struct tuple *t = data.tuples[i++];
		uint32_t size;
		benchmark::DoNotOptimize(*tuple_data_range(t, &size));
		benchmark::DoNotOptimize(size);
	}
	total_count += i;
	state.SetItemsProcessed(total_count);
}

BENCHMARK_TEMPLATE(tuple_access_data_range, FORMAT_BASIC);

// benchmark of access of non-indexed field.
template<data_format F>
static void
tuple_access_unindexed_field(benchmark::State& state)
{
	BenchDataSimple<F> data;
	size_t i = 0;
	size_t total_count = 0;
	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			i = 0;
		}
		struct tuple *t = data.tuples[i++];
		benchmark::DoNotOptimize(*tuple_field(t, 3));
	}
	total_count += i;
	state.SetItemsProcessed(total_count);
}

BENCHMARK_TEMPLATE(tuple_access_unindexed_field, FORMAT_BASIC);

// benchmark of access of indexed field.
template<data_format F>
static void
tuple_access_indexed_field(benchmark::State& state)
{
	BenchDataSimple<F> data;
	size_t i = 0;
	size_t total_count = 0;
	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			i = 0;
		}
		struct tuple *t = data.tuples[i];
		benchmark::DoNotOptimize(*tuple_field(t, 4));
		++i;
	}
	total_count += i;
	state.SetItemsProcessed(total_count);
}

BENCHMARK_TEMPLATE(tuple_access_indexed_field, FORMAT_BASIC);

// benchmark of tuple compare.
template<data_format F>
static void
tuple_tuple_compare(benchmark::State& state)
{
	BenchDataSimple<F> data;
	size_t i = 0;
	size_t j = 0;
	size_t total_count = 0;
	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			i = 0;
		}
		if (j >= NUM_TEST_TUPLES)
			j -= NUM_TEST_TUPLES;
		struct tuple *t1 = data.tuples[i];
		struct tuple *t2 = data.tuples[j];
		benchmark::DoNotOptimize(tuple_compare(t1, 0, t2, 0, data.kd));
		++i;
		j += 3;
	}
	total_count += i;
	state.SetItemsProcessed(total_count);
}

BENCHMARK_TEMPLATE(tuple_tuple_compare, FORMAT_BASIC);

static void
tuple_tuple_hash_impl(benchmark::State &state, struct key_def *kd)
{
	struct tuple_format *format = TupleFormat<FORMAT_BASIC>::make(kd);
	TestTuples<FORMAT_BASIC> tuples(format);
	size_t i = 0;
	size_t total_count = 0;
	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			i = 0;
		}
		benchmark::DoNotOptimize(tuple_hash(tuples[i], kd));
		++i;
	}
	total_count += i;
	state.SetItemsProcessed(total_count);
	tuple_format_unref(format);
}

static void
tuple_tuple_hash_fast_one_uint(benchmark::State &state)
{
	struct key_part_def kdp = key_part_def_default;
	kdp.fieldno = 0;
	kdp.type = FIELD_TYPE_UNSIGNED;
	struct key_def *kd = key_def_new(&kdp, 1, 0);
	tuple_tuple_hash_impl(state, kd);
	key_def_delete(kd);
}

BENCHMARK(tuple_tuple_hash_fast_one_uint);

static void
tuple_tuple_hash_fast_multiple_fields(benchmark::State &state)
{
	struct key_part_def kdp[2];
	kdp[0] = key_part_def_default;
	kdp[0].fieldno = 0;
	kdp[0].type = FIELD_TYPE_UNSIGNED;
	kdp[1] = key_part_def_default;
	kdp[1].fieldno = 1;
	kdp[1].type = FIELD_TYPE_STRING;
	struct key_def *kd = key_def_new(kdp, 2, 0);
	tuple_tuple_hash_impl(state, kd);
	key_def_delete(kd);
}

BENCHMARK(tuple_tuple_hash_fast_multiple_fields);

static void
tuple_tuple_hash_slow(benchmark::State &state)
{
	struct key_part_def kdp[2];

	kdp[0] = key_part_def_default;
	kdp[0].fieldno = 0;
	kdp[0].type = FIELD_TYPE_UNSIGNED;
	kdp[0].is_nullable = true;
	kdp[0].nullable_action = ON_CONFLICT_ACTION_NONE;

	kdp[1] = key_part_def_default;
	kdp[1].fieldno = 1;
	kdp[1].type = FIELD_TYPE_STRING;
	kdp[1].is_nullable = true;
	kdp[1].nullable_action = ON_CONFLICT_ACTION_NONE;

	struct key_def *kd = key_def_new(kdp, 2, 0);
	tuple_tuple_hash_impl(state, kd);
	key_def_delete(kd);
}

BENCHMARK(tuple_tuple_hash_slow);

// benchmark of tuple hints compare.
template<data_format F>
static void
tuple_tuple_compare_hint(benchmark::State& state)
{
	BenchDataSimple<F> data;
	size_t i = 0;
	size_t j = 0;
	size_t total_count = 0;
	for (auto _ : state) {
		if (i == NUM_TEST_TUPLES) {
			total_count += i;
			i = 0;
		}
		if (j >= NUM_TEST_TUPLES)
			j -= NUM_TEST_TUPLES;
		struct tuple *t1 = data.tuples[i];
		struct tuple *t2 = data.tuples[j];
		hint_t h1 = tuple_hint(t1, data.kd);
		hint_t h2 = tuple_hint(t2, data.kd);
		benchmark::DoNotOptimize(tuple_compare(
			t1, h1, t2, h2, data.kd));
		++i;
		j += 3;
	}
	total_count += i;
	state.SetItemsProcessed(total_count);
}

BENCHMARK_TEMPLATE(tuple_tuple_compare_hint, FORMAT_BASIC);

int
main(int argc, char **argv)
{
	MemtxEngine::instance();
	::benchmark::Initialize(&argc, argv);
	::benchmark::RunSpecifiedBenchmarks();
}

#include "debug_warning.h"
