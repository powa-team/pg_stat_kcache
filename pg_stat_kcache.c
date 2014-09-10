/*
 * pg_stat_kcache (kcache)
 *
 * Provides basic statistics about real I/O done by the filesystem layer.
 * This way, calculating a real hit-ratio is doable.
 *
 * Large portions of code freely inspired by pg_stat_plans. Thanks to Peter
 * Geoghegan for this great extension.
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 */
#include <sys/time.h>
#include <sys/resource.h>

#include "postgres.h"

#include <unistd.h>

#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"


PG_MODULE_MAGIC;

#if PG_VERSION_NUM >= 90300
#define PGSK_DUMP_FILE		"pg_stat/pg_stat_kcache.stat"
#else
#define PGSK_DUMP_FILE		"global/pg_stat_kcache.stat"
#endif

#define PG_STAT_KCACHE_COLS 6

static const uint32 PGSK_FILE_HEADER = 0x0d756e0e;

struct	rusage own_rusage;

/*
 * Current read and write values, as returned by getrusage
*/
typedef struct pgskCounters
{
	int64				current_reads;
	int64				current_writes;
	struct	timeval		current_utime;
	struct	timeval		current_stime;
} pgskCounters;
pgskCounters	counters = {0, 0, {0, 0}, {0, 0}};

static int	pgsk_max_db;	/* max # db to store */

/*
 * Statistics per database
 */
typedef struct pgskEntry
{
	Oid			dbid;	/* database Oid */
	int64		reads[CMD_NOTHING];		/* number of physical reads per database and per statement kind */
	int64		writes[CMD_NOTHING];	/* number of physical writes per database and per statement kind */
	float8		utime[CMD_NOTHING];		/* CPU user time per database and per statement kind */
	float8		stime[CMD_NOTHING];		/* CPU system time per database and per statement kind */
	slock_t		mutex;	/* protects the counters only */
} pgskEntry;

/*
 * Global shared state
 */
typedef struct pgskSharedState
{
	LWLockId	lock;			/* protects hashtable search/modification */
} pgskSharedState;


/* saved hook address in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* Links to shared memory state */
static pgskSharedState *pgsk = NULL;
static pgskEntry *pgskEntries = NULL;

/*--- Functions --- */

void	_PG_init(void);
void	_PG_fini(void);

Datum	pg_stat_kcache_reset(PG_FUNCTION_ARGS);
Datum	pg_stat_kcache(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_stat_kcache_reset);
PG_FUNCTION_INFO_V1(pg_stat_kcache);

static Size pgsk_memsize(void);

static void pgsk_shmem_startup(void);
static void pgsk_shmem_shutdown(int code, Datum arg);
static void pgsk_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgsk_ExecutorEnd(QueryDesc *queryDesc);
static void entry_reset(void);
static void entry_store(Oid dbid, int64 reads, int64 writes,
						double utime,
						double stime,
						CmdType operation);

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "This module can only be loaded via shared_preload_libraries");
		return;
	}

	/*
	 * Define (or redefine) custom GUC variables.
	 */
	DefineCustomIntVariable( "pg_stat_kcache.max_db",
		"Define how many databases will be stored.",
							NULL,
							&pgsk_max_db,
							200,
							1,
							INT_MAX,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);


	RequestAddinShmemSpace(pgsk_memsize());
	RequestAddinLWLocks(1);

	/* Install hook */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgsk_shmem_startup;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgsk_ExecutorStart;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgsk_ExecutorEnd;
}

void
_PG_fini(void)
{
	/* uninstall hook */
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorEnd_hook = prev_ExecutorEnd;
	shmem_startup_hook = prev_shmem_startup_hook;
}

static void
pgsk_shmem_startup(void)
{
	bool		found;
	FILE		*file;
	int			i,t;
	uint32		header;
	int32		num;
	pgskEntry	*entry;
	pgskEntry	*buffer = NULL;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	pgsk = NULL;

	/* Create or attach to the shared memory state */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	/* global access lock */
	pgsk = ShmemInitStruct("pg_stat_kcache",
					sizeof(pgskSharedState),
					&found);

	if (!found)
	{
		/* First time through ... */
		pgsk->lock = LWLockAssign();
	}

	/* allocate stats shared memory structure */
	pgskEntries = ShmemInitStruct("pg_stat_kcache stats",
					sizeof(pgskEntry) * pgsk_max_db,
					&found);

	if (!found)
	{
		entry_reset();
	}

	LWLockRelease(AddinShmemInitLock);

	if (!IsUnderPostmaster)
		on_shmem_exit(pgsk_shmem_shutdown, (Datum) 0);

	/* Load stat file, don't care about locking */
	file = AllocateFile(PGSK_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno == ENOENT)
			return;			/* ignore not-found error */
		goto error;
	}

	buffer = (pgskEntry *) palloc(sizeof(pgskEntry));

	/* check is header is valid */
	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		header != PGSK_FILE_HEADER)
		goto error;

	/* get number of entries */
	if (fread(&num, sizeof(int32), 1, file) != 1)
		goto error;

	entry = pgskEntries;
	for (i = 0; i < num ; i++)
	{
		if (fread(buffer, offsetof(pgskEntry, mutex), 1, file) != 1)
			goto error;

		entry->dbid = buffer->dbid;
		for (t = 0; t < CMD_NOTHING; t++)
		{
			entry->reads[t] = buffer->reads[t];
			entry->writes[t] = buffer->writes[t];
			entry->utime[t] = buffer->utime[t];
			entry->stime[t] = buffer->stime[t];
		}
		/* don't initialize spinlock, already done */
		entry++;
	}

	pfree(buffer);
	FreeFile(file);

	/*
	 * Remove the file so it's not included in backups/replication slaves,
	 * etc. A new file will be written on next shutdown.
	 */
	unlink(PGSK_DUMP_FILE);

	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read pg_stat_kcache file \"%s\": %m",
					PGSK_DUMP_FILE)));
	if (buffer)
		pfree(buffer);
	if (file)
		FreeFile(file);
	/* delete bogus file, don't care of errors in this case */
	unlink(PGSK_DUMP_FILE);
}

/*
 * shmem_shutdown hook: dump statistics into file.
 *
 */
static void
pgsk_shmem_shutdown(int code, Datum arg)
{
	FILE	*file;
	int32	num_entries;
	int	i;
	pgskEntry	*entry;

	/* Don't try to dump during a crash. */
	if (code)
		return;

	if (!pgsk)
		return;

	file = AllocateFile(PGSK_DUMP_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	if (fwrite(&PGSK_FILE_HEADER, sizeof(uint32), 1, file) != 1)
		goto error;

	entry = pgskEntries;
	num_entries = 0;
	while (num_entries < pgsk_max_db)
	{
		if (entry->dbid == InvalidOid)
			break;
		entry++;
		num_entries++;
	}

	if (fwrite(&num_entries, sizeof(int32), 1, file) != 1)
		goto error;

	entry = pgskEntries;
	for (i = 0; i < num_entries; i++)
	{
		if (fwrite(entry, offsetof(pgskEntry, mutex), 1, file) != 1)
			goto error;
		entry++;
	}

	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	/*
	 * Rename file inplace
	 */
	if (rename(PGSK_DUMP_FILE ".tmp", PGSK_DUMP_FILE) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not rename pg_stat_kcache file \"%s\": %m",
						PGSK_DUMP_FILE ".tmp")));

	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read pg_stat_kcache file \"%s\": %m",
					PGSK_DUMP_FILE)));

	if (file)
		FreeFile(file);
	unlink(PGSK_DUMP_FILE);
}

static Size pgsk_memsize(void)
{
	Size	size;

	size = MAXALIGN(sizeof(pgskSharedState)) + MAXALIGN(sizeof(pgskEntry)) * pgsk_max_db;

	return size;
}


/*
 * support functions
 */

static void
entry_store(Oid dbid, int64 reads, int64 writes,
						double utime,
						double stime,
						CmdType operation)
{
	int i = 0;
	pgskEntry *entry;
	bool found = false;

	entry = pgskEntries;

	LWLockAcquire(pgsk->lock, LW_SHARED);

	while (i < pgsk_max_db && !found)
	{
		if (entry->dbid == dbid || entry->dbid == InvalidOid) {
			SpinLockAcquire(&entry->mutex);
			entry->dbid = dbid;
			entry->reads[operation] += reads;
			entry->writes[operation] += writes;
			entry->utime[operation] += utime;
			entry->stime[operation] += stime;
			SpinLockRelease(&entry->mutex);
			found = true;
			break;
		}
		entry++;
		i++;
	}

	/* if there's no more room, then raise a warning */
	if (!found)
	{
		elog(WARNING, "pg_stat_kcache: no more free entry for dbid %d", dbid);
	}

	LWLockRelease(pgsk->lock);
	return;
}

static void entry_reset(void)
{
	int i,t;
	pgskEntry	*entry;

	LWLockAcquire(pgsk->lock, LW_EXCLUSIVE);

	/* Mark all entries with InvalidOid */
	entry = pgskEntries;
	for (i = 0; i < pgsk_max_db ; i++)
	{
		entry->dbid = InvalidOid;
		for(t = 0; t < CMD_NOTHING; t++)
		{
			entry->reads[t] = 0;
			entry->writes[t] = 0;
			entry->utime[t] = 0.0;
			entry->stime[t] = 0.0;
		}
		SpinLockInit(&entry->mutex);
		entry++;
	}

	LWLockRelease(pgsk->lock);
	return;
}

/*
 * Hooks
 */

static void
pgsk_ExecutorStart (QueryDesc *queryDesc, int eflags)
{
	/* capture kernel usage stats in own_rusage */
	getrusage(RUSAGE_SELF, &own_rusage);

	/* store current number of block reads */
	counters.current_reads = own_rusage.ru_inblock;
	counters.current_writes = own_rusage.ru_oublock;
	counters.current_utime.tv_sec = own_rusage.ru_utime.tv_sec;
	counters.current_utime.tv_usec = own_rusage.ru_utime.tv_usec;
	counters.current_stime.tv_sec = own_rusage.ru_stime.tv_sec;
	counters.current_stime.tv_usec = own_rusage.ru_stime.tv_usec;

	/* give control back to PostgreSQL */
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
pgsk_ExecutorEnd (QueryDesc *queryDesc)
{
	Oid dbid;

	dbid = MyDatabaseId;
	double utime;
	double stime;

	/* capture kernel usage stats in own_rusage */
	getrusage(RUSAGE_SELF, &own_rusage);

	/* Compute CPU time delta */
	utime = ( (double) own_rusage.ru_utime.tv_sec + (double) own_rusage.ru_utime.tv_usec / 1000000.0 )
		- ( (double) counters.current_utime.tv_sec + (double) counters.current_utime.tv_usec / 1000000.0 );
	stime = ( (double) own_rusage.ru_stime.tv_sec + (double) own_rusage.ru_stime.tv_usec / 1000000.0 )
		- ( (double) counters.current_stime.tv_sec + (double) counters.current_stime.tv_usec / 1000000.0 );

	/* store current number of block reads and writes */
	entry_store(dbid, own_rusage.ru_inblock - counters.current_reads,
			own_rusage.ru_oublock - counters.current_writes,
			utime,
			stime,
			queryDesc->operation
	);

	/* give control back to PostgreSQL */
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

}

/*
 * Reset statistics.
 */
Datum
pg_stat_kcache_reset(PG_FUNCTION_ARGS)
{
	if (!pgsk)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_kcache must be loaded via shared_preload_libraries")));

	entry_reset();
	PG_RETURN_VOID();
}

/*
 * Show the amount of reads and writes per sessions
 *
Datum
pg_stat_kcache_session(PG_FUNCTION_ARGS)
{

}
*/
Datum
pg_stat_kcache(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext	per_query_ctx;
	MemoryContext	oldcontext;
	pgskEntry		*entry;
	TupleDesc		tupdesc;
	Tuplestorestate	*tupstore;
	int				i = 0;


	if (!pgsk)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_kcache must be loaded via shared_preload_libraries")));
	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
							"allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	LWLockAcquire(pgsk->lock, LW_SHARED);

	entry = pgskEntries;
	while (i < pgsk_max_db)
	{
		int t;
		for (t = 0; t < CMD_NOTHING; t++)
		{
			Datum		values[PG_STAT_KCACHE_COLS];
			bool		nulls[PG_STAT_KCACHE_COLS];
			int			j = 0;

			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			if (entry->dbid == InvalidOid)
				break;
			SpinLockAcquire(&entry->mutex);
			values[j++] = ObjectIdGetDatum(entry->dbid);
			values[j++] = Int64GetDatumFast(entry->reads[t]);
			values[j++] = Int64GetDatumFast(entry->writes[t]);
			values[j++] = Float8GetDatumFast(entry->utime[t]);
			values[j++] = Float8GetDatumFast(entry->stime[t]);
			switch (t)
			{
				case CMD_UNKNOWN:
					values[j++] = CStringGetTextDatum("UNKNOWN");
				break;
				case CMD_SELECT:
					values[j++] = CStringGetTextDatum("SELECT");
				break;
				case CMD_UPDATE:
					values[j++] = CStringGetTextDatum("UPDATE");
				break;
				case CMD_INSERT:
					values[j++] = CStringGetTextDatum("INSERT");
				break;
				case CMD_DELETE:
					values[j++] = CStringGetTextDatum("DELETE");
				break;
				case CMD_UTILITY:
					values[j++] = CStringGetTextDatum("UTILITY");
				break;
				case CMD_NOTHING:
					values[j++] = CStringGetTextDatum("NOTHING");
				break;
				default:
					values[j++] = CStringGetTextDatum("ERROR");
				break;
			}
			SpinLockRelease(&entry->mutex);

			Assert(j == PG_STAT_PLAN_COLS);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
		entry++;
		i++;
	}

	LWLockRelease(pgsk->lock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

