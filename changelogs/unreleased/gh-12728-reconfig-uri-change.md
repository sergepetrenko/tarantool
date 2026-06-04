## bugfix/replication

* Fixed replication reconfiguration ignoring changes to a peer URI (for example
  login, password, address or other parameters) when the new URI still led to
  the same replica. Now the connection is reestablished with the new URI as
  soon as it is provided via configuration (gh-12728).
