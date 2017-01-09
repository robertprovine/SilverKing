// OpenDir.c

///////////////
// includes

#include "OpenDir.h"
#include "OpenDirUpdate.h"
#include "OpenDirWriter.h"
#include "ReconciliationSet.h"

#include <string.h>

#include <set>

////////////
// defines

#define OD_MIN_PREFETCH_INTERVAL_MILLIS	(1 * 60 * 1000)
#define OD_TRIGGER_INTERVAL_MILLIS	4


///////////////////
// public globals

OpenDirWriter	*od_odw;


//////////////////////
// private functions

static void od_merge_DirData_pendingUpdates(OpenDir *od);
static void od_clear_pending_updates(OpenDir *od);


///////////////
// implementation

OpenDir *od_new(const char *path, DirData *dirData) {
	OpenDir	*od;
	int	i;
	pthread_mutexattr_t mutexAttr;

	od = (OpenDir *)mem_alloc(1, sizeof(OpenDir));
    srfsLog(LOG_FINE, "od_new:\t%s\n", path);
	strncpy(od->path, path, SRFS_MAX_PATH_LENGTH);
	
	if (dirData != NULL) {
		od->dd = dd_dup(dirData);
	} else {
		od->dd = dd_new_empty();
	}
	
	od->mutex = &od->mutexInstance;
	pthread_mutexattr_init(&mutexAttr);
	pthread_mutexattr_settype(&mutexAttr, PTHREAD_MUTEX_RECURSIVE);	
    if (pthread_mutex_init(od->mutex, &mutexAttr) != 0) {
        fatalError("\n mutex init failed", __FILE__, __LINE__);
	}
	od->lastUpdateMillis = curTimeMillis();
	
	return od;
}

void od_delete(OpenDir **od) {
	if (od != NULL && *od != NULL) {
		int	i;
		
		if ((*od)->dd != NULL) {
			dd_delete(&(*od)->dd);
		}
		
		// Free pending updates
		for (i = 0; i < (*od)->numPendingUpdates; i++) {
			mem_free((void **)&(*od)->pendingUpdates[i].name);
		}
		mem_free((void **)&(*od)->pendingUpdates);
		
		pthread_mutex_destroy((*od)->mutex);
		mem_free((void **)od);
	} else {
		fatalError("bad ptr in od_delete");
	}
}

uint64_t od_getLastUpdateMillis(OpenDir *od) {
	return od->lastUpdateMillis;
}

uint64_t od_getElapsedSinceLastUpdateMillis(OpenDir *od) {
	return curTimeMillis() - od->lastUpdateMillis;
}

void od_mark_deleted(OpenDir *od) {
    // FUTURE - for now we can't free data or reconciliation may go bad
    /*
    pthread_mutex_lock(od->mutex);
	dd_delete(&od->dd);
	od->dd = dd_new_empty();
	od_clear_pending_updates(od);
    pthread_mutex_unlock(od->mutex);
    */
}

// lock must be held
static void od_clear_pending_updates(OpenDir *od) {
	if (od->numPendingUpdates > 0) {
		int	i;
		
		for (i = 0; i < od->numPendingUpdates; i++) {
			mem_free((void **)&od->pendingUpdates[i].name);
		}
		mem_free((void **)&od->pendingUpdates);
		od->numPendingUpdates = 0;
	}
}

DirData *od_get_DirData(OpenDir *od, int clearPending) {
	DirData	*dd;
	
    pthread_mutex_lock(od->mutex);	
	
    dd = dd_process_updates(od->dd, od->pendingUpdates, od->numPendingUpdates);	
	if (clearPending) {
		if (od->dd) {
			dd_delete(&od->dd);
		}
		od->dd = dd;
		dd = dd_dup(dd);
		od_clear_pending_updates(od);
	}
	
    pthread_mutex_unlock(od->mutex);	
	return dd;
}

// lock must be held
static void od_merge_DirData_pendingUpdates(OpenDir *od) {
	DirData	*dd;
	
    dd = dd_process_updates(od->dd, od->pendingUpdates, od->numPendingUpdates);	
	if (od->dd) {
		dd_delete(&od->dd);
	}
	od->dd = dd;
	od_clear_pending_updates(od);
}

static void od_add_update(OpenDir *od, char *name, int type, uint64_t version) {
	int	i;
	int	existing;
	
	existing = FALSE;
	// lock
    pthread_mutex_lock(od->mutex);	
	for (i = 0; i < od->numPendingUpdates; i++) {
		if (!strncmp(name, od->pendingUpdates[i].name, SRFS_MAX_PATH_LENGTH)) {
			existing = TRUE;
			if (version > od->pendingUpdates[i].version) {
				srfsLog(LOG_FINE, "Modifying update %s to %d, %d", name, type, version);
				odu_modify(&od->pendingUpdates[i], type, version);
			} else {
				srfsLog(LOG_FINE, "Ignoring stale update %s %d, %d", name, type, version);
			}
			break;
		}
	}
	if (!existing) {
		// queue an update
		mem_realloc((void **)&od->pendingUpdates, od->numPendingUpdates, od->numPendingUpdates + 1, sizeof(OpenDirUpdate));
		odu_init(&od->pendingUpdates[od->numPendingUpdates], type, version, name);
		od->numPendingUpdates++;
		rcst_add_to_reconciliation_set(od->path);
		od->needsReconciliation = TRUE;
	}
	// unlock
    pthread_mutex_unlock(od->mutex);	
}

void od_rm_entry(OpenDir *od, char *name, uint64_t version) {
	od_add_update(od, name, ODU_T_DELETION, version);
}

void od_add_entry(OpenDir *od, char *name, uint64_t version) {
	od_add_update(od, name, ODU_T_ADDITION, version);
}

void od_add_DirData(OpenDir *od, DirData *dd, SKMetaData *metaData) {
	uint64_t	metaDataVersion;
	uint64_t	_curTimeMillis;
    int         deleteOnExit;
	
	srfsLog(LOG_FINE, "od_add_DirData %llx %llx", od, dd);
	_curTimeMillis = curTimeMillis();
    if (dd == NULL) {
        dd = dd_new_empty();
        deleteOnExit = TRUE;
        metaDataVersion = _curTimeMillis;
    } else {
        deleteOnExit = FALSE;
        metaDataVersion = metaData->getVersion();
    }
    pthread_mutex_lock(od->mutex);	
	if (od->ddVersion < metaDataVersion && od->lastMergedVersion != metaDataVersion) {
		MergeResult	mr;
		
		od_merge_DirData_pendingUpdates(od);
		mr = dd_merge(od->dd, dd);
		od->lastMergedVersion = metaDataVersion;
		if (!mr.dd1NotIn0) {
			srfsLog(LOG_FINE, "No updates from KVS data");
			// Note, mr.dd is always NULL in this case
			//od->needsReconciliation = FALSE; 
		} else {
			dd_delete(&od->dd);
			od->dd = mr.dd;
			od->lastUpdateMillis = _curTimeMillis;
			od->ddVersion = metaDataVersion;
		}
		if (mr.dd0NotIn1) {
			srfsLog(LOG_FINE, "Updating KVS version with local data");
			odw_write_dir(od_odw, od->path, od);
		}
		if (!mr.dd1NotIn0 && !mr.dd0NotIn1) {
			srfsLog(LOG_INFO, "getID() %d get_pid() %d", metaData != NULL ? metaData->getCreator()->getID() : 0, get_pid());
			if (metaData != NULL && metaData->getCreator()->getID() != get_pid()) {
				rcst_remove_from_reconciliation_set(od->path);
				od->needsReconciliation = FALSE; 
			}
			//if (od->needsReconciliation > 0) {
			//	--od->needsReconciliation;
			//}
		} else {
			od->needsReconciliation = TRUE;
		}
	} else {
		srfsLog(LOG_FINE, "Ignoring stale or duplicate update %llx %l", od, od->ddVersion);
	}
    pthread_mutex_unlock(od->mutex);	
    if (deleteOnExit) {
        dd_delete(&dd);
    }
	srfsLog(LOG_FINE, "out od_add_DirData %llx %llx", od, dd);
}

int od_updates_pending(OpenDir *od) {
	return od->numPendingUpdates > 0;
}

int od_set_queued_for_write(OpenDir *od, int queuedForWrite) {
	int	markedByThisCall;
	
    pthread_mutex_lock(od->mutex);	
	if (od->queuedForWrite != queuedForWrite) {
		markedByThisCall = TRUE;
		od->queuedForWrite = queuedForWrite;
	} else {
		markedByThisCall = FALSE;
	}
    pthread_mutex_unlock(od->mutex);	
	return markedByThisCall;
}

void od_display(OpenDir *od, FILE *file) {
	int	i;
	
    pthread_mutex_lock(od->mutex);
	fprintf(file, "******************\n");
	dd_display(od->dd);
	fprintf(file, "\nnumPendingUpdates: %d\n", od->numPendingUpdates);
	for (i = 0; i < od->numPendingUpdates; i++) {
		fprintf(file, "\t\t%s\t%d\n", od->pendingUpdates[i].name, od->pendingUpdates[i].type);
	}
	fprintf(file, "******************\n");
    pthread_mutex_unlock(od->mutex);
}

int od_record_get_attr(OpenDir *od, char *child, uint64_t curTime) {
	/*
	int	doPrefetch;
	
	doPrefetch = (curTime - od->lastGetAttr <= OD_TRIGGER_INTERVAL_MILLIS) 
			&& (curTime - od->lastPrefetch > OD_MIN_PREFETCH_INTERVAL_MILLIS);
	od->lastGetAttr = curTime;
	if (doPrefetch) {
		od->lastPrefetch = curTime;
	}
	return doPrefetch;
	*/
	// FIXME - temp disable prefetching - seems to be memory corruption caused in the current prefetch code
	// resolve this as prefetch provides a critical speed boost
	return FALSE;
}