## 2.1.2 (2020-07-08)

**Bugfix**:

  - Accumulate counters for parallel workers too (Julien Rouhaud, thanks to
    Atsushi Torikoshi for the report)

## 2.1.1 (2018-07-28)

**Bugfix**:

  - Fix usage increase, used to keep the most frequent entries in memory
    (Julien Rouhaud)

**Miscellaneous**:

  - Allow PG_CONFIG value to be found on command-line (edechaux)
  - Warn users about incorrect GUC (Julien Rouhaud)
  - Add debian packaging (Julien Rouhaud)

## 2.1.0 (2018-07-17)

**NOTE**: This version requires a change to the on-disk format.  After
installing the new version restarting PostgreSQL, any previously accumulated
statistics will be reset.

  - Add support for architecture that don't provide getrusage(2), such as
    windows.  Only user time and system time will be available on such
    platforms (Julien Rouhaud).
  - Expose more fields of getrusage(2).  Depending on the platform, some of
    these fields are not maintained (Julien Rouhaud).
  - Add a workaround for sampling problems with getrusage(), new parameter
    pg_stat_kcache.linux_hz is added.  By default, this parameter is discovered
    at server startup (Ronan Dunklau).
  - Add compatibility with PostgreSQL 11 (Thomas Reiss)
  - Fix issue when concurrently created entries for the same user, db and
    queryid could lost some execution counters (Mael Rimbault)
  - Do not install docs anymore (Ronan Dunklau)

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
