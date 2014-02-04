/*
 * pg_stat_kcache (kcache)
 *
 * Provides basic statistics about real I/O done by the filesystem layer.
 * This way, calculating a real hit-ratio is doable.
 *
 * Large portions of code freely inspired by pg_stat_plans. Thanks to Peter
 * Geoghegan for this great extension.
 *
 * 2014, Thomas Reiss, PostgreSQL licence
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


PG_MODULE_MAGIC;

#define PGSK_DUMP_FILE  "global/pg_stat_kcache.stat"
#define MAX_DB_ENTRIES 200
#define PG_STAT_KCACHE_COLS 2

static const uint32 PGSK_FILE_HEADER = 0x0d756e0e;

struct	rusage own_rusage;
int64	current_reads = 0;

/*
 * Statistics per database
 */
typedef struct pgskEntry
{
	Oid			dbid;	/* database Oid */
	int64		reads;		/* number of physical reads per database */
	slock_t		mutex;		/* protects the counters only */
} pgskEntry;

/*
 * Global shared state
 */
typedef struct pgskSharedState
{
	LWLockId	lock;			/* protects hashtable search/modification */
	int			query_size;		/* max query length in bytes */
} pgskSharedState;


/* saved hook address in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* Links to shared memory state */
static pgskSharedState *pgsk = NULL;
static pgskEntry *pgskEntries = NULL;

/*--- Functions --- */

void            _PG_init(void);
void            _PG_fini(void);

Datum	pg_stat_kcache_reset(PG_FUNCTION_ARGS);
Datum	pg_stat_kcache(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_stat_kcache_reset);
PG_FUNCTION_INFO_V1(pg_stat_kcache);

static Size pgsk_memsize(void);

static void pgsk_shmem_startup(void);
static void pgsk_shmem_shutdown(int code, Datum arg);
static void pgsk_ExecutorEnd(QueryDesc *queryDesc);
static void entry_reset(void);
static void entry_store(Oid dbid, int64 reads);

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "This module can only be loaded via shared_preload_libraries");
		return;
	}

	RequestAddinShmemSpace(pgsk_memsize());
	RequestAddinLWLocks(1);

	/* Install hook */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgsk_shmem_startup;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgsk_ExecutorEnd;
}

void
_PG_fini(void)
{
	/* uninstall hook */
	ExecutorEnd_hook = prev_ExecutorEnd;
	shmem_startup_hook = prev_shmem_startup_hook;
}

static void
pgsk_shmem_startup(void)
{
	bool		found;
	FILE		*file;
	int			i;
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
					sizeof(pgskEntry) * MAX_DB_ENTRIES,
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
			return;                         /* ignore not-found error */
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
		entry->reads = buffer->reads;
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
	while (num_entries < MAX_DB_ENTRIES)
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

	size = MAXALIGN(sizeof(pgskSharedState)) + MAXALIGN(sizeof(pgskEntry)) * MAX_DB_ENTRIES;
	
	return size;
}


/*
 * support functions
 */

static void
entry_store(Oid dbid, int64 reads)
{
	int i = 0;
	pgskEntry *entry;
	bool found = false;

	entry = pgskEntries;

	LWLockAcquire(pgsk->lock, LW_SHARED);

	while (i < MAX_DB_ENTRIES && !found)
	{
		if (entry->dbid == dbid || entry->dbid == InvalidOid) {
			SpinLockAcquire(&entry->mutex);
			entry->dbid = dbid;
			entry->reads += reads;
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
	int i;
	pgskEntry  *entry;

	LWLockAcquire(pgsk->lock, LW_EXCLUSIVE);

	/* Mark all entries with InvalidOid */
	entry = pgskEntries;
	for (i = 0; i < MAX_DB_ENTRIES ; i++)
	{
		entry->dbid = InvalidOid;
		entry->reads = 0;
		SpinLockInit(&entry->mutex);
		entry++;
	}

	LWLockRelease(pgsk->lock);
	return;
}

/*
 * Hook
 */

static void
pgsk_ExecutorEnd (QueryDesc *queryDesc)
{
	Oid dbid;

	dbid = MyDatabaseId;

	/* capture kernel usage stats in own_rusage */
	getrusage(RUSAGE_SELF, &own_rusage);

	/* store current number of block reads */
	entry_store(dbid, own_rusage.ru_inblock - current_reads);
	current_reads = own_rusage.ru_inblock;

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
 * Show the amount of reads per sessions
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
	while (i < MAX_DB_ENTRIES)
	{
		Datum           values[PG_STAT_KCACHE_COLS];
			bool            nulls[PG_STAT_KCACHE_COLS];
			int                     j = 0;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		if (entry->dbid == InvalidOid)
			break;
		SpinLockAcquire(&entry->mutex);
		values[j++] = ObjectIdGetDatum(entry->dbid);
		values[j++] = Int64GetDatumFast(entry->reads);
		SpinLockRelease(&entry->mutex);

		Assert(j == PG_STAT_PLAN_COLS);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		entry++;
		j++;
	}

	LWLockRelease(pgsk->lock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

