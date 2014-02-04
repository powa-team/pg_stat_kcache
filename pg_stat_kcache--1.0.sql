-- This program is open source, licensed under the PostgreSQL License.
-- For license terms, see the LICENSE file.
--
-- Copyright (C) 2014: Dalibo

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_kernel" to load this file. \quit

SET client_encoding = 'UTF8';
SET check_function_bodies = true;

CREATE FUNCTION pg_stat_kcache(OUT dbid oid, OUT reads bigint, OUT operation text)
    RETURNS SETOF record
    LANGUAGE c COST 1000
    AS '$libdir/pg_stat_kcache', 'pg_stat_kcache';

CREATE FUNCTION pg_stat_kcache_reset()
    RETURNS void
    LANGUAGE c COST 1000
    AS '$libdir/pg_stat_kcache', 'pg_stat_kcache_reset';

/* ru_inblock block size is 512 bytes with Linux
 * see http://lkml.indiana.edu/hypermail/linux/kernel/0703.2/0937.html
 */
CREATE VIEW pg_stat_kcache AS
SELECT dbid, datname,
       reads*512/(current_setting('block_size')::integer) AS reads,
       operation
  FROM pg_stat_kcache()
  JOIN pg_database
    ON oid=dbid;

GRANT SELECT ON pg_stat_kcache TO public;
GRANT ALL ON FUNCTION pg_stat_kcache() TO public;
REVOKE ALL ON FUNCTION pg_stat_kcache_reset() FROM public;
