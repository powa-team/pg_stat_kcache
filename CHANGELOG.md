## 2.0.3 (WIP)
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
