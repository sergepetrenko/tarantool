## bugfix/replication

* Significantly improved the replication performance by batching rows to be sent
  before the dispatch. Batch size in bytes may be controlled by the new tweak
  option `relay_stream_flush_size` (default 16 kb) (gh-10161).
