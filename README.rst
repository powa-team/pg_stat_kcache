pg_stat_kcache
==============

Features
--------

Gathers statistics about real reads and writes done by the filesystem layer.
It is provided in the form of an extension for PostgreSQL >= 9.4., and requires
pg_stat_statements extension to be installed. PostgreSQL 9.4 or more is
required as previous version of provided pg_stat_statements didn't expose the
queryid field.

Installation
============

Compiling
---------

The module can be built using the standard PGXS infrastructure. For this to work, the
``pg_config`` program must be available in your $PATH. Instruction to install follows::

 git clone https://github.com/dalibo/pg_stat_kcache.git
 cd pg_stat_kcache
 make
 make install

PostgreSQL setup
----------------

The extension is now available. But, as it requires some shared memory to hold
its counters, the module must be loaded at PostgreSQL startup. Thus, you must
add the module to ``shared_preload_libraries`` in your ``postgresql.conf``. You need a
server restart to take the change into account.

Add the following parameters into you ``postgresql.conf``::

 # postgresql.conf
 shared_preload_libraries = 'pg_stat_kcache'

Once your PostgreSQL cluster is restarted, you can install the extension in every
database where you need to access the statistics::

 mydb=# CREATE EXTENSION pg_stat_kcache;

Usage
=====

pg_stat_kcache create several objects.

pg_stat_kcache view
-------------------

+-------------+-------------------+-----------------------------------------------------+
| Name        | Type              | Description                                         |
+=============+===================+=====================================================+
| datname     | name              | Name of the database                                |
+-------------+-------------------+-----------------------------------------------------+
| reads       | bigint            + Number of blocks read by the filesystem layer       |
+-------------+-------------------+-----------------------------------------------------+
| reads_blks  | bigint            + Number of 8K blocks read by the filesystem layer    |
+-------------+-------------------+-----------------------------------------------------+
| writes      | bigint            + Number of blocks written by the filesystem layer    |
+-------------+-------------------+-----------------------------------------------------+
| writes_blks | bigint            + Number of 8K blocks written by the filesystem layer |
+-------------+-------------------+-----------------------------------------------------+
| user_time   | double precision  + User CPU time used                                  |
+-------------+-------------------+-----------------------------------------------------+
| system_time | double precision  + System CPU time used                                |
+-------------+-------------------+-----------------------------------------------------+

pg_stat_kcache_detail view
--------------------------

+-------------+-------------------+-----------------------------------------------------+
| Name        | Type              | Description                                         |
+=============+===================+=====================================================+
| query       | text              | Query text                                          |
+-------------+-------------------+-----------------------------------------------------+
| datname     | name              | Database name                                       |
+-------------+-------------------+-----------------------------------------------------+
| rolname     | name              | Role name                                           |
+-------------+-------------------+-----------------------------------------------------+
| reads       | bigint            + Number of bytes read by the filesystem layer        |
+-------------+-------------------+-----------------------------------------------------+
| reads_blks  | bigint            + Number of 8K blocks read by the filesystem layer    |
+-------------+-------------------+-----------------------------------------------------+
| writes      | bigint            + Number of bytes written by the filesystem layer     |
+-------------+-------------------+-----------------------------------------------------+
| writes_blks | bigint            + Number of 8K blocks written by the filesystem layer |
+---------------+-----------------+-----------------------------------------------------+
| user_time   | double precision  + User CPU time used                                  |
+---------------+-----------------+-----------------------------------------------------+
| system_time | double precision  + System CPU time used                                |
+-------------+-------------------+-----------------------------------------------------+

pg_stat_kcache_reset function
-----------------------------

Resets the statistics gathered by pg_stat_kcache. Can be called by superusers::

 pg_stat_kcache_reset()


pg_stat_kcache function
-----------------------

This function is a set-returning functions that dumps the containt of the counters
of the shared memory structure. This function is used by the pg_stat_kcache view.
The function can be called by any user::

 SELECT * FROM pg_stat_kcache();

It provides the following columns:

+-------------+-------------------+--------------------------------------------------+
| Name        | Type              | Description                                      |
+============+====================+==================================================+
| queryid     | bigint            | pg_stat_statements' query identifier             |
+-------------+-------------------+--------------------------------------------------+
| userid      | oid               | Database OID                                     |
+-------------+-------------------+--------------------------------------------------+
| dbid        | oid               | Database OID                                     |
+-------------+-------------------+--------------------------------------------------+
| reads       | bigint            + Number of bytes read by the filesystem layer     |
+-------------+-------------------+--------------------------------------------------+
| writes      | bigint            + Number of bytes written by the filesystem layer  |
+---------------+-----------------+--------------------------------------------------+
| user_time   | double precision  + User CPU time used                               |
+-------------+-------------------+--------------------------------------------------+
| system_time | double precision  + System CPU time use                              |
+-------------+-------------------+--------------------------------------------------+

Bugs and limitations
====================

No known bugs.

We assume that a kernel block is 512 bytes. This is true for Linux, but may not
be the case for another Unix implementation.

See: http://lkml.indiana.edu/hypermail/linux/kernel/0703.2/0937.html

Authors
=======

pg_stat_kcache is an original development from Thomas Reiss, with large portions
of code inspired from pg_stat_plans. Julien Rouhaud also contributed some parts of
the extension.

Thanks goes to Peter Geoghegan for providing much inspiration with pg_stat_plans
so we could write this extension quite straightforward.

License
=======

pg_stat_kcache is free software distributed under the PostgreSQL license.

Copyright (c) 2014, dalibo.

