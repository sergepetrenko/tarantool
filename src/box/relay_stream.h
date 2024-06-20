#pragma once
/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2024, Tarantool AUTHORS, please see AUTHORS file.
 */

#include "iostream.h"
#include "small/lsregion.h"
#include "xrow.h"
#include "memory.h"

/** Written data size after which the stream should be flushed. */
extern size_t relay_stream_flush_size;

/**
 * A structure encapsulating writes made by relay. Collects the rows into a
 * buffer and flushes it to the network as soon as its size reaches a specific
 * threshold.
 */
struct relay_stream {
	/** A region storing rows buffered for despatch. */
	struct lsregion lsregion;
	/** A growing identifier for lsregion allocations. */
	int64_t lsr_id;
	/** A savepoint used between flushes. */
	struct lsregion_svp flush_pos;
};

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** Initialize the stream. */
static inline void
relay_stream_create(struct relay_stream *stream)
{
	lsregion_create(&stream->lsregion, &runtime);
	stream->lsr_id = LSLAB_NOT_USED_ID;
	lsregion_svp_create(&stream->flush_pos);
}

static inline void
relay_stream_destroy(struct relay_stream *stream)
{
	lsregion_destroy(&stream->lsregion);
}

/** Test whether the stream is full and needs a flush. */
static inline bool
relay_stream_needs_flush(struct relay_stream *stream)
{
	return lsregion_used(&stream->lsregion) > relay_stream_flush_size;
}

/** Write a row to the stream. */
void
relay_stream_write(struct relay_stream *stream, const struct xrow_header *row);

/** Flush the stream contents to the given iostream. */
int
relay_stream_flush(struct relay_stream *stream, struct iostream *io);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */
