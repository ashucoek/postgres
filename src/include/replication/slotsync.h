/*-------------------------------------------------------------------------
 *
 * slotsync.h
 *	  Exports for slot synchronization.
 *
 * Portions Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * src/include/replication/slotsync.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLOTSYNC_H
#define SLOTSYNC_H

#include "replication/walreceiver.h"

extern PGDLLIMPORT bool sync_replication_slots;

/*
 * GUCs needed by slot sync worker to connect to the primary
 * server and carry on with slots synchronization.
 */
extern PGDLLIMPORT char *PrimaryConnInfo;
extern PGDLLIMPORT char *PrimarySlotName;

typedef enum SlotSyncSkipReason
{
	SLOT_SYNC_SKIP_NONE,		/* No skip */
	SLOT_SYNC_SKIP_STANDBY_BEHIND,	/* Standby is behind the remote slot */
	SLOT_SYNC_SKIP_REMOTE_BEHIND,	/* Remote slot is behind the local slot */
	SLOT_SYNC_SKIP_NO_CONSISTENT_SNAPSHOT	/* Standby could not reach a
											 * consistent snapshot */
}			SlotSyncSkipReason;

extern char *CheckAndGetDbnameFromConninfo(void);
extern bool ValidateSlotSyncParams(int elevel);

pg_noreturn extern void ReplSlotSyncWorkerMain(const void *startup_data, size_t startup_data_len);

extern void ShutDownSlotSync(void);
extern bool SlotSyncWorkerCanRestart(void);
extern bool IsSyncingReplicationSlots(void);
extern Size SlotSyncShmemSize(void);
extern void SlotSyncShmemInit(void);
extern void SyncReplicationSlots(WalReceiverConn *wrconn);

#endif							/* SLOTSYNC_H */
