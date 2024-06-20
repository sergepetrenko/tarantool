#include "relay_stream.h"
#include "tweaks.h"
#include "coio.h"
#include "errinj.h"

uint64_t relay_stream_flush_size = 16384;
TWEAK_UINT(relay_stream_flush_size);

void
relay_stream_write(struct relay_stream *stream, const struct xrow_header *row)
{
	size_t used = region_used(&fiber()->gc);
	int iovcnt;
	struct iovec iov[XROW_IOVMAX];
	xrow_to_iovec(row, iov, &iovcnt);
	size_t len = 0;
	for (int i = 0; i < iovcnt; i++) {
		len += iov[i].iov_len;
	}
	char *data = xlsregion_alloc(&stream->lsregion, len, ++stream->lsr_id);

	for (int i = 0; i < iovcnt; i++) {
		memcpy(data, iov[i].iov_base, iov[i].iov_len);
		data += iov[i].iov_len;
	}
	region_truncate(&fiber()->gc, used);
}

int
relay_stream_flush(struct relay_stream *stream, struct iostream *io)
{
	ssize_t to_flush = lsregion_used(&stream->lsregion);
	/*
	 * Might flush more than requested if data is added to the buffer
	 * during the coio_writev yield.
	 */
	while (to_flush > 0) {
		struct iovec iov[IOV_MAX];
		int iovcnt = lengthof(iov);
		int64_t gc_id = lsregion_to_iovec(&stream->lsregion, iov,
						   &iovcnt, &stream->flush_pos);
		ssize_t written = coio_writev(io, iov, iovcnt, 0);
		if (written < 0)
			return -1;
		to_flush -= written;
		lsregion_gc(&stream->lsregion, gc_id);
	}
	struct errinj *inj = errinj(ERRINJ_RELAY_TIMEOUT, ERRINJ_DOUBLE);
	if (inj != NULL && inj->dparam > 0)
		fiber_sleep(inj->dparam);
	return 0;
}
