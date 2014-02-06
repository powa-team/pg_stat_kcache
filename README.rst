pg_stat_kcache
==============

Features
--------

Gathers statistics about real reads done by the filesystem layer. It is provided
in the form of an extension for PostgreSQL > 9.1. It should work with lower versions
of PostgreSQL but the support code has to be reworked to do so.

Installation
============

Compiling
---------

The module can be built using the standard PGXS infrastructure. For this to work, the
``pg_config`` program must be available in your $PATH.

  tar xvfz pg_stat_kcache.tar.gz
  cd pg_stat_kcache.tar.gz
  make
  make install

PostgreSQL setup
----------------

The extension is now available. But, as it requires some shared memory to hold
its counters, the module must be loaded at PostgreSQL startup. Thus, you must
add the module to shared_preload_libraries in your postgresql.conf. You need a
server restart to take the change into account.

  # postgresql.conf
  shared_preload_libraries = 'pg_stat_kcache'
  pg_stat_kcache.max_db = 200

You can change the parameter "pg_stat_kcache_directory" (default 200) to define
how many databases pg_stat_kcache will keep track of.

Note that this extension should work with other, like pg_stat_plans or pg_stat_statements.

Once your PostgreSQL cluster is restarted, you can install the extension in every
database where you need to access the statistics :

  mydb=# CREATE EXTENSION pg_stat_kcache;

Usage
=====

pg_stat_kcache create several objects.

pg_stat_kcache view
-------------------

+-----------+---------+----------------------------------------------------+
| Name      | Type    | Description                                        |
+===========+=========+====================================================+
| dbid      | oid     | Database OID                                       |
+-----------+---------+----------------------------------------------------+
| reads     | bigint  + Number of 8K blocks read by the filesystem layer   |
+-----------+---------+----------------------------------------------------+
| operation | text    | Kind of statement                                  |
+-----------+---------+----------------------------------------------------+

This function assumes that the filesystem block size is 512 bytes.

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

It provides the following columns :

+---------+---------+-----------------------------------------------+
| Name    | Type    | Description                                   |
+=========+=========+===============================================+
| dbid    | oid     | Database OID                                  |
+---------+---------+-----------------------------------------------+
| reads   | bigint  + Number of blocks read by the filesystem layer |
+---------+---------+-----------------------------------------------+

Note that the block size is not equal to PostgreSQL block size. A Linux kernel
accounts them as 512 byte blocks.

Bugs and limitations
====================

No known bugs.

The number of database followed by the extension is limited by an internal
constant set to 200. If you have more than 200 databases, please change define
MAX_DB_ENTRIES in pg_stat_kcache.c. An upcoming version of this module may add
a GUC value to control this more easily.



