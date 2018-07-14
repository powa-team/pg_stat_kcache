/*----------------
 * pg_stat_kcache
 *
 * Provides basic statistics about real I/O done by the filesystem layer.
 * This way, calculating a real hit-ratio is doable.  Also provides basis
 * statistics about CPU usage.
 *
 * Large portions of code freely inspired by pg_stat_plans. Thanks to Peter
 * Geoghegan for this great extension.
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 */

#include "postgres.h"

#include <unistd.h>

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/resource.h>
#endif

#ifndef HAVE_GETRUSAGE
#include "rusagestub.h"
#endif

#include "access/hash.h"
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

/* In PostgreSQL 11, queryid becomes a uint64 internally.
 */
#if PG_VERSION_NUM >= 110000
typedef uint64 pgsk_queryid;
#else
typedef uint32 pgsk_queryid;
#endif

#define PG_STAT_KCACHE_COLS 7
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every pgsk_entry_dealloc */
#define STICKY_DECREASE_FACTOR	(0.50)	/* factor for sticky entries */
#define USAGE_DEALLOC_PERCENT	5		/* free this % of entries at once */
#define USAGE_INIT				(1.0)	/* including initial planning */

/* ru_inblock block size is 512 bytes with Linux
 * see http://lkml.indiana.edu/hypermail/linux/kernel/0703.2/0937.html
 */
#define RUSAGE_BLOCK_SIZE	512			/* Size of a block for getrusage() */

#define TIMEVAL_DIFF(start, end) ((double) end.tv_sec + (double) end.tv_usec / 1000000.0) \
	- ((double) start.tv_sec + (double) start.tv_usec / 1000000.0)

static const uint32 PGSK_FILE_HEADER = 0x0d756e0f;

static struct	rusage rusage_start;

/*
 * Current getrusage counters.
 *
 * For platform without getrusage support, we rely on postgres implementation
 * defined in rusagestub.h, which only supports user and system time.
*/
typedef struct pgskCounters
{
	int64			calls;		/* # of times executed */
	double			usage;		/* usage factor */
	/* These fields are only used for platform with HAVE_GETRUSAGE defined */
#ifdef HAVE_GETRUSAGE
	int64			reads;	/* Physical block reads */
	int64			writes;	/* Physical block writes */
#endif
	/* These fields are always used */
	float8			utime;	/* CPU user time */
	float8			stime;	/* CPU system time */
} pgskCounters;

static int	pgsk_max = 0;	/* max #queries to store. pg_stat_statements.max is used */

/*
 * Hashtable key that defines the identity of a hashtable entry.  We use the
 * same hash as pg_stat_statements
 */
typedef struct pgskHashKey
{
	Oid			userid;			/* user OID */
	Oid			dbid;			/* database OID */
	pgsk_queryid		queryid;		/* query identifier */
} pgskHashKey;

/*
 * Statistics per database
 */
typedef struct pgskEntry
{
	pgskHashKey		key;		/* hash key of entry - MUST BE FIRST */
	pgskCounters	counters;	/* statistics for this query */
	slock_t			mutex;		/* protects the counters only */
} pgskEntry;

/*
 * Global shared state
 */
typedef struct pgskSharedState
{
	LWLockId	lock;					/* protects hashtable search/modification */
	double		cur_median_usage;		/* current median usage in hashtable */
} pgskSharedState;


/* saved hook address in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/* Links to shared memory state */
static pgskSharedState *pgsk = NULL;
static HTAB *pgsk_hash = NULL;

/*--- Functions --- */

void	_PG_init(void);
void	_PG_fini(void);

extern PGDLLEXPORT Datum	pg_stat_kcache_reset(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum	pg_stat_kcache(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_stat_kcache_reset);
PG_FUNCTION_INFO_V1(pg_stat_kcache);

static void pgsk_setmax(void);
static Size pgsk_memsize(void);

static void pgsk_shmem_startup(void);
static void pgsk_shmem_shutdown(int code, Datum arg);
static void pgsk_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgsk_ExecutorEnd(QueryDesc *queryDesc);
static pgskEntry *pgsk_entry_alloc(pgskHashKey *key, bool sticky);
static void pgsk_entry_dealloc(void);
static void pgsk_entry_reset(void);
static void pgsk_entry_store(pgsk_queryid queryId, pgskCounters counters);
static uint32 pgsk_hash_fn(const void *key, Size keysize);
static int	pgsk_match_fn(const void *key1, const void *key2, Size keysize);


static bool pgsk_assign_linux_hz_check_hook(int *newval, void **extra, GucSource source);

static int	pgsk_linux_hz;

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "This module can only be loaded via shared_preload_libraries");
		return;
	}
	DefineCustomIntVariable("pg_stat_kcache.linux_hz",
				"Inform pg_stat_kcache of the linux CONFIG_HZ config option",
		  "This is used by pg_stat_kcache to compensate for sampling errors "
							"in getrusage due to the kernel adhering to its ticks. The default value, -1, "
							"tries to guess it at startup. ",
							&pgsk_linux_hz,
							-1,
							-1,
							INT_MAX,
							PGC_USERSET,
							0,
							pgsk_assign_linux_hz_check_hook,
							NULL,
							NULL);

	/* set pgsk_max if needed */
	pgsk_setmax();
	RequestAddinShmemSpace(pgsk_memsize());
#if PG_VERSION_NUM >= 90600
	RequestNamedLWLockTranche("pg_stat_kcache", 1);
#else
	RequestAddinLWLocks(1);
#endif

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

static bool
pgsk_assign_linux_hz_check_hook(int *newval, void **extra, GucSource source)
{
	int			val = *newval;
	struct rusage myrusage;
	struct timeval previous_value;

	/* In that case we try to guess it */
	if (val == -1)
	{
		elog(LOG, "Auto detecting pg_stat_kcache.linux_hz parameter...");
		getrusage(RUSAGE_SELF, &myrusage);
		previous_value = myrusage.ru_utime;
		while (myrusage.ru_utime.tv_usec == previous_value.tv_usec &&
			   myrusage.ru_utime.tv_sec == previous_value.tv_sec)
		{
			getrusage(RUSAGE_SELF, &myrusage);
		}
		*newval = (int) (1 / ((myrusage.ru_utime.tv_sec - previous_value.tv_sec) +
		   (myrusage.ru_utime.tv_usec - previous_value.tv_usec) / 1000000.));
		elog(LOG, "pg_stat_kcache.linux_hz is set to %d", *newval);
	}
	return true;
}


static void
pgsk_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;
	FILE		*file;
	int			i;
	uint32		header;
	int32		num;
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
#if PG_VERSION_NUM >= 90600
		pgsk->lock = &(GetNamedLWLockTranche("pg_stat_kcache"))->lock;
#else
		pgsk->lock = LWLockAssign();
#endif
	}

	/* set pgsk_max if needed */
	pgsk_setmax();

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(pgskHashKey);
	info.entrysize = sizeof(pgskEntry);
	info.hash = pgsk_hash_fn;
	info.match = pgsk_match_fn;

	/* allocate stats shared memory hash */
	pgsk_hash = ShmemInitHash("pg_stat_kcache hash",
							  pgsk_max, pgsk_max,
							  &info,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	LWLockRelease(AddinShmemInitLock);

	if (!IsUnderPostmaster)
		on_shmem_exit(pgsk_shmem_shutdown, (Datum) 0);

	/*
	 * Done if some other process already completed our initialization.
	 */
	if (found)
		return;

	/* Load stat file, don't care about locking */
	file = AllocateFile(PGSK_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno == ENOENT)
			return;			/* ignore not-found error */
		goto error;
	}

	/* check is header is valid */
	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		header != PGSK_FILE_HEADER)
		goto error;

	/* get number of entries */
	if (fread(&num, sizeof(int32), 1, file) != 1)
		goto error;

	for (i = 0; i < num ; i++)
	{
		pgskEntry	temp;
		pgskEntry  *entry;

		if (fread(&temp, sizeof(pgskEntry), 1, file) != 1)
			goto error;

		/* Skip loading "sticky" entries */
		if (temp.counters.calls == 0)
			continue;

		entry = pgsk_entry_alloc(&temp.key, false);

		/* copy in the actual stats */
		entry->counters = temp.counters;
		/* don't initialize spinlock, already done */
	}

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
	HASH_SEQ_STATUS hash_seq;
	int32	num_entries;
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

	num_entries = hash_get_num_entries(pgsk_hash);

	if (fwrite(&num_entries, sizeof(int32), 1, file) != 1)
		goto error;

	hash_seq_init(&hash_seq, pgsk_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (fwrite(entry, sizeof(pgskEntry), 1, file) != 1)
		{
			/* note: we assume hash_seq_term won't change errno */
			hash_seq_term(&hash_seq);
			goto error;
		}
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

/*
 * Retrieve pg_stat_statement.max GUC value and store it into pgsk_max, since
 * we want to store the same number of entries as pg_stat_statements. Don't do
 * anything if pgsk_max is already set.
 */
static void pgsk_setmax(void)
{
	const char *pgss_max;
	const char *name = "pg_stat_statements.max";

	if (pgsk_max != 0)
		return;

	pgss_max = GetConfigOption(name, true, false);

	/*
	 * Retrieving pg_stat_statements.max can fail if pgss is loaded after pgsk
	 * in shared_preload_libraries.  Hint user in case this happens.
	 */
	if (!pgss_max)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized configuration parameter \"%s\"",
						name),
				 errhint("make sure pg_stat_statements is loaded,\n"
					 "and make sure pg_stat_kcache is present after pg_stat_statements"
					 " in the shared_preload_libraries setting")));

	pgsk_max = atoi(pgss_max);
}

static Size pgsk_memsize(void)
{
	Size	size;

	Assert(pgsk_max != 0);

	size = MAXALIGN(sizeof(pgskSharedState));
	size = add_size(size, hash_estimate_size(pgsk_max, sizeof(pgskEntry)));

	return size;
}


/*
 * support functions
 */

static void
pgsk_entry_store(pgsk_queryid queryId, pgskCounters counters)
{
	volatile pgskEntry *e;

	pgskHashKey key;
	pgskEntry  *entry;

	/* Safety check... */
	if (!pgsk || !pgsk_hash)
		return;

	/* Set up key for hashtable search */
	key.userid = GetUserId();
	key.dbid = MyDatabaseId;
	key.queryid = queryId;

	/* Lookup the hash table entry with shared lock. */
	LWLockAcquire(pgsk->lock, LW_SHARED);

	entry = (pgskEntry *) hash_search(pgsk_hash, &key, HASH_FIND, NULL);

	/* Create new entry, if not present */
	if (!entry)
	{
		/* Need exclusive lock to make a new hashtable entry - promote */
		LWLockRelease(pgsk->lock);
		LWLockAcquire(pgsk->lock, LW_EXCLUSIVE);

		/* OK to create a new hashtable entry */
		entry = pgsk_entry_alloc(&key, false);
	}

	/*
	 * Grab the spinlock while updating the counters (see comment about
	 * locking rules at the head of the file)
	 */
	e = (volatile pgskEntry *) entry;

	SpinLockAcquire(&e->mutex);

	/* "Unstick" entry if it was previously sticky */
	if (e->counters.calls == 0)
		e->counters.usage = USAGE_INIT;

	e->counters.calls += 1;
#ifdef HAVE_GETRUSAGE
	e->counters.reads += counters.reads;
	e->counters.writes += counters.writes;
#endif
	e->counters.utime += counters.utime;
	e->counters.stime += counters.stime;

	SpinLockRelease(&e->mutex);

	LWLockRelease(pgsk->lock);
}

/*
 * Allocate a new hashtable entry.
 * caller must hold an exclusive lock on pgsk->lock
 */
static pgskEntry *pgsk_entry_alloc(pgskHashKey *key, bool sticky)
{
	pgskEntry  *entry;
	bool		found;

	/* Make space if needed */
	while (hash_get_num_entries(pgsk_hash) >= pgsk_max)
		pgsk_entry_dealloc();

	/* Find or create an entry with desired hash code */
	entry = (pgskEntry *) hash_search(pgsk_hash, key, HASH_ENTER, &found);

	if (!found)
	{
		/* New entry, initialize it */

		/* reset the statistics */
		memset(&entry->counters, 0, sizeof(pgskCounters));
		/* set the appropriate initial usage count */
		entry->counters.usage = sticky ? pgsk->cur_median_usage : USAGE_INIT;
		/* re-initialize the mutex each time ... we assume no one using it */
		SpinLockInit(&entry->mutex);
	}

	return entry;
}

/*
 * qsort comparator for sorting into increasing usage order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double		l_usage = (*(pgskEntry *const *) lhs)->counters.usage;
	double		r_usage = (*(pgskEntry *const *) rhs)->counters.usage;

	if (l_usage < r_usage)
		return -1;
	else if (l_usage > r_usage)
		return +1;
	else
		return 0;
}

/*
 * Deallocate least used entries.
 * Caller must hold an exclusive lock on pgsk->lock.
 */
static void
pgsk_entry_dealloc(void)
{
	HASH_SEQ_STATUS hash_seq;
	pgskEntry **entries;
	pgskEntry  *entry;
	int			nvictims;
	int			i;

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values.
	 */
	entries = palloc(hash_get_num_entries(pgsk_hash) * sizeof(pgskEntry *));

	i = 0;
	hash_seq_init(&hash_seq, pgsk_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entries[i++] = entry;
		/* "Sticky" entries get a different usage decay rate. */
		if (entry->counters.calls == 0)
			entry->counters.usage *= STICKY_DECREASE_FACTOR;
		else
			entry->counters.usage *= USAGE_DECREASE_FACTOR;
	}

	qsort(entries, i, sizeof(pgskEntry *), entry_cmp);

	if (i > 0)
	{
		/* Record the (approximate) median usage */
		pgsk->cur_median_usage = entries[i / 2]->counters.usage;
	}

	nvictims = Max(10, i * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		hash_search(pgsk_hash, &entries[i]->key, HASH_REMOVE, NULL);
	}

	pfree(entries);
}

static void pgsk_entry_reset(void)
{
	HASH_SEQ_STATUS hash_seq;
	pgskEntry  *entry;

	LWLockAcquire(pgsk->lock, LW_EXCLUSIVE);

	hash_seq_init(&hash_seq, pgsk_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(pgsk_hash, &entry->key, HASH_REMOVE, NULL);
	}

	LWLockRelease(pgsk->lock);
}

/*
 * Hooks
 */

static void
pgsk_ExecutorStart (QueryDesc *queryDesc, int eflags)
{
	/* capture kernel usage stats in rusage_start */
	getrusage(RUSAGE_SELF, &rusage_start);

	/* give control back to PostgreSQL */
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
pgsk_ExecutorEnd (QueryDesc *queryDesc)
{
	pgsk_queryid queryId;
	struct rusage rusage_end;
	pgskCounters counters;

	/* capture kernel usage stats in rusage_end */
	getrusage(RUSAGE_SELF, &rusage_end);

	queryId = queryDesc->plannedstmt->queryId;

	/* Compute CPU time delta */
	counters.utime = TIMEVAL_DIFF(rusage_start.ru_utime, rusage_end.ru_utime);
	counters.stime = TIMEVAL_DIFF(rusage_start.ru_stime, rusage_end.ru_stime);

	if (queryDesc->totaltime)
	{
		/* Make sure stats accumulation is done */
		InstrEndLoop(queryDesc->totaltime);

		/*
		 * We only consider values greater than 3 * linux tick, otherwise the
		 * bias is too big
		 */
		if (queryDesc->totaltime->total < (3. / pgsk_linux_hz))
		{
			counters.stime = 0;
			counters.utime = queryDesc->totaltime->total;
		}
	}

#ifdef HAVE_GETRUSAGE
	/* Compute the rest of the counters */
	counters.reads = rusage_end.ru_inblock - rusage_start.ru_inblock;
	counters.writes = rusage_end.ru_oublock - rusage_start.ru_oublock;
#endif

	/* store current number of block reads and writes */
	pgsk_entry_store(queryId, counters);

	/* give control back to PostgreSQL */
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

}

/*
 * Calculate hash value for a key
 */
static uint32
pgsk_hash_fn(const void *key, Size keysize)
{
	const pgskHashKey *k = (const pgskHashKey *) key;

	return hash_uint32((uint32) k->userid) ^
		hash_uint32((uint32) k->dbid) ^
		hash_uint32((uint32) k->queryid);
}

/*
 * Compare two keys - zero means match
 */
static int
pgsk_match_fn(const void *key1, const void *key2, Size keysize)
{
	const pgskHashKey *k1 = (const pgskHashKey *) key1;
	const pgskHashKey *k2 = (const pgskHashKey *) key2;

	if (k1->userid == k2->userid &&
		k1->dbid == k2->dbid &&
		k1->queryid == k2->queryid)
		return 0;
	else
		return 1;
}

/*
 * Reset statistics.
 */
PGDLLEXPORT Datum
pg_stat_kcache_reset(PG_FUNCTION_ARGS)
{
	if (!pgsk)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_kcache must be loaded via shared_preload_libraries")));

	pgsk_entry_reset();
	PG_RETURN_VOID();
}

PGDLLEXPORT Datum
pg_stat_kcache(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext	per_query_ctx;
	MemoryContext	oldcontext;
	TupleDesc		tupdesc;
	Tuplestorestate	*tupstore;
	HASH_SEQ_STATUS hash_seq;
	pgskEntry		*entry;


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

	/* Switch into long-lived context to construct returned data structures */
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

	hash_seq_init(&hash_seq, pgsk_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum			values[PG_STAT_KCACHE_COLS];
		bool			nulls[PG_STAT_KCACHE_COLS];
		pgskCounters	tmp;
		int				i = 0;
#ifdef HAVE_GETRUSAGE
		int64			reads, writes;
#endif

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* copy counters to a local variable to keep locking time short */
		{
			volatile pgskEntry *e = (volatile pgskEntry *) entry;

			SpinLockAcquire(&e->mutex);
			tmp = e->counters;
			SpinLockRelease(&e->mutex);
		}

		/* Skip entry if unexecuted (ie, it's a pending "sticky" entry) */
		if ( tmp.calls == 0)
			continue;

		values[i++] = Int64GetDatum(entry->key.queryid);
		values[i++] = ObjectIdGetDatum(entry->key.userid);
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
#ifdef HAVE_GETRUSAGE
		reads = tmp.reads * RUSAGE_BLOCK_SIZE;
		writes = tmp.writes * RUSAGE_BLOCK_SIZE;
		values[i++] = Int64GetDatumFast(reads);
		values[i++] = Int64GetDatumFast(writes);
#else
		nulls[i++] = true; /* reads */
		nulls[i++] = true; /* writes */
#endif
		values[i++] = Float8GetDatumFast(tmp.utime);
		values[i++] = Float8GetDatumFast(tmp.stime);

		Assert(i == PG_STAT_KCACHE_COLS);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	LWLockRelease(pgsk->lock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

