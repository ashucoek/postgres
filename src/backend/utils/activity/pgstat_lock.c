/* -------------------------------------------------------------------------
 *
 * pgstat_lock.c
 *	  Implementation of lock statistics.
 *
 * This file contains the implementation of lock statistics. It is kept separate
 * from pgstat.c to enforce the line between the statistics access / storage
 * implementation and the details about individual types of statistics.
 *
 * Copyright (c) 2021-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_lock.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"

static PgStat_PendingLock PendingLockStats;
static bool have_lockstats = false;

PgStat_Lock *
pgstat_fetch_stat_lock(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_LOCK);

	return &pgStatLocal.snapshot.lock;
}

/*
 * Simpler wrapper of pgstat_lock_flush_cb()
 */
void
pgstat_lock_flush(bool nowait)
{
	(void) pgstat_lock_flush_cb(nowait);
}

/*
 * Flush out locally pending lock statistics
 *
 * If no stats have been recorded, this function returns false.
 *
 * If nowait is true, this function returns true if the lock could not be
 * acquired. Otherwise, return false.
 */
bool
pgstat_lock_flush_cb(bool nowait)
{
	LWLock	   *lcktype_lock;
	PgStat_LockEntry *lck_shstats;
	bool		lock_not_acquired = false;

	if (!have_lockstats)
		return false;

	for (int i = 0; i <= LOCKTAG_LAST_TYPE; i++)
	{
		lcktype_lock = &pgStatLocal.shmem->lock.locks[i];
		lck_shstats =
			&pgStatLocal.shmem->lock.stats.stats[i];

		if (!nowait)
			LWLockAcquire(lcktype_lock, LW_EXCLUSIVE);
		else if (!LWLockConditionalAcquire(lcktype_lock, LW_EXCLUSIVE))
		{
			lock_not_acquired = true;
			continue;
		}

#define LOCKSTAT_ACC(fld) \
	(lck_shstats->fld += PendingLockStats.stats[i].fld)
		LOCKSTAT_ACC(requests);
		LOCKSTAT_ACC(waits);
		LOCKSTAT_ACC(timeouts);
		LOCKSTAT_ACC(deadlock_timeouts);
		LOCKSTAT_ACC(deadlocks);
		LOCKSTAT_ACC(fastpath);
#undef LOCKSTAT_ACC

		LWLockRelease(lcktype_lock);
	}

	memset(&PendingLockStats, 0, sizeof(PendingLockStats));

	have_lockstats = false;

	return lock_not_acquired;
}


void
pgstat_lock_init_shmem_cb(void *stats)
{
	PgStatShared_Lock *stat_shmem = (PgStatShared_Lock *) stats;

	for (int i = 0; i <= LOCKTAG_LAST_TYPE; i++)
		LWLockInitialize(&stat_shmem->locks[i], LWTRANCHE_PGSTATS_DATA);
}

void
pgstat_lock_reset_all_cb(TimestampTz ts)
{
	for (int i = 0; i <= LOCKTAG_LAST_TYPE; i++)
	{
		LWLock	   *lcktype_lock = &pgStatLocal.shmem->lock.locks[i];
		PgStat_LockEntry *lck_shstats = &pgStatLocal.shmem->lock.stats.stats[i];

		LWLockAcquire(lcktype_lock, LW_EXCLUSIVE);

		/*
		 * Use the lock in the first lock type PgStat_LockEntry to protect the
		 * reset timestamp as well.
		 */
		if (i == 0)
			pgStatLocal.shmem->lock.stats.stat_reset_timestamp = ts;

		memset(lck_shstats, 0, sizeof(*lck_shstats));
		LWLockRelease(lcktype_lock);
	}
}

void
pgstat_lock_snapshot_cb(void)
{
	for (int i = 0; i <= LOCKTAG_LAST_TYPE; i++)
	{
		LWLock	   *lcktype_lock = &pgStatLocal.shmem->lock.locks[i];
		PgStat_LockEntry *lck_shstats = &pgStatLocal.shmem->lock.stats.stats[i];
		PgStat_LockEntry *lck_snap = &pgStatLocal.snapshot.lock.stats[i];

		LWLockAcquire(lcktype_lock, LW_SHARED);

		/*
		 * Use the lock in the first lock type PgStat_LockEntry to protect the
		 * reset timestamp as well.
		 */
		if (i == 0)
			pgStatLocal.snapshot.lock.stat_reset_timestamp =
				pgStatLocal.shmem->lock.stats.stat_reset_timestamp;

		/* using struct assignment due to better type safety */
		*lck_snap = *lck_shstats;
		LWLockRelease(lcktype_lock);
	}
}

#define PGSTAT_COUNT_LOCK_FUNC(stat)					\
void													\
CppConcat(pgstat_count_lock_,stat)(uint8 locktag_type)	\
{														\
	Assert(locktag_type <= LOCKTAG_LAST_TYPE);			\
	PendingLockStats.stats[locktag_type].stat++;		\
	have_lockstats = true;								\
	pgstat_report_fixed = true;							\
}

/* pgstat_count_lock_requests */
PGSTAT_COUNT_LOCK_FUNC(requests)

/* pgstat_count_lock_waits */
PGSTAT_COUNT_LOCK_FUNC(waits)

/* pgstat_count_lock_timeouts */
PGSTAT_COUNT_LOCK_FUNC(timeouts)

/* pgstat_count_lock_deadlock_timeouts */
PGSTAT_COUNT_LOCK_FUNC(deadlock_timeouts)

/* pgstat_count_lock_deadlocks */
PGSTAT_COUNT_LOCK_FUNC(deadlocks)

/* pgstat_count_lock_fastpath */
PGSTAT_COUNT_LOCK_FUNC(fastpath)
