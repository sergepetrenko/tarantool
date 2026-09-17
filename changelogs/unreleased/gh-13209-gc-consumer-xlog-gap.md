## bugfix/replication

* Fixed WAL garbage collection being blocked by a replica reconnecting with
  a vclock older than the upstream's retained WALs (gh-13209).
