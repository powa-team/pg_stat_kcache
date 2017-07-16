## 2.0.4 (WIP)
  - Do not install docs anymore
  - Add a workaround for sampling problems with getrusage(), new parameter
    pg_stat_kcache.linux_hz is added.  By default, this parameter is discovered
    at server startup (Ronan Dunklau).
  - Fix issue when concurrently created entries for the same user, db and
    queryid could lost some execution counters

## 2.0.3 (2016-10-03)
  - Add PG 9.6 compatibility
  - Fix issues in shared memory estimation, which could prevent starting
    postgres or reduce the amount of possible locks (thanks to Jean-Sébastien
    BACQ for the report)
  - Add hint of possible reasons pgss.max could not be retrieved, which could
    prevent starting postgres

## 2.0.2 (2015-03-17)

  - Fix another bug with 32 bits builds (thanks to Alain Delorme for reporting it)

## 2.0.1 (2015-03-16)

  - Fix a bug with 32 bits builds (thanks to Alain Delorme for reporting it)

## 2.0 (2015-01-30)

  - Handle stats per database, user, query

## 1.0 (2014-02-26)

  - Initial release
