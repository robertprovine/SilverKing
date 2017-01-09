// DirDataReader.c

/////////////
// includes

#include "ActiveOp.h"
#include "ActiveOpRef.h"
#include "DirDataReadRequest.h"
#include "DirDataReader.h"
#include "FileID.h"
#include "G2OutputDir.h"
#include "G2TaskOutputReader.h"
#include "SRFSConstants.h"
#include "Util.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <set>
#include <map>
#include <vector>
#include <string>

////////////
// defines

#define DDR_UPDATE_INTERVAL_MILLIS	(10 * 1000)


///////////////////////
// private prototypes

static void ddr_update_DirData_in_cache(DirDataReadRequest *ddrr, DirData *dd, SKMetaData *metaData);
static void ddr_store_DirData_as_OpenDir_in_cache(DirDataReadRequest *ddrr, DirData *dd);
static void ddr_process_dht_batch(void **requests, int numRequests, int curThreadIndex);
static int _ddr_get_OpenDir(DirDataReader *ddr, char *path, OpenDir **od, int createIfNotFound);


/////////////////
// private data


///////////////////
// implementation

DirDataReader *ddr_new(SRFSDHT *sd, ResponseTimeStats *rtsDirData, OpenDirCache *openDirCache) {
	DirDataReader *ddr;

	ddr = (DirDataReader*)mem_alloc(1, sizeof(DirDataReader));
	ddr->dirDataQueueProcessor = qp_new_batch_processor(ddr_process_dht_batch, __FILE__, __LINE__, 
								DDR_DHT_QUEUE_SIZE, ABQ_FULL_DROP, DDR_DHT_THREADS, DDR_MAX_BATCH_SIZE);
	ddr->sd = sd;
	ddr->rtsDirData = rtsDirData;
	ddr->openDirCache = openDirCache;
	try {
		int	i;
		
		for (i = 0; i < DDR_DHT_THREADS; i++) {
			SKNamespace	*ns;
			SKNamespacePerspectiveOptions *nspOptions;
			SKGetOptions	*pGetOpt;
			
			ddr->pSession[i] = sd_new_session(ddr->sd);
			ns = ddr->pSession[i]->getNamespace(SKFS_DIR_NS);
			nspOptions = ns->getDefaultNSPOptions();
			pGetOpt = nspOptions->getDefaultGetOptions();			
			pGetOpt = pGetOpt->retrievalType(VALUE_AND_META_DATA);
			nspOptions = nspOptions->defaultGetOptions(pGetOpt);
			ddr->ansp[i] = ns->openAsyncPerspective(nspOptions);
			delete ns;
		}
	} catch(SKClientException & ex){
		srfsLog(LOG_ERROR, "ddr_new exception opening namespace %s: what: %s\n", SKFS_DIR_NS, ex.what());
		fatalError("exception in ddr_new", __FILE__, __LINE__ );
	}
	
	return ddr;
}

void ddr_delete(DirDataReader **ddr) {
	if (ddr != NULL && *ddr != NULL) {
		(*ddr)->dirDataQueueProcessor->running = FALSE;
		odc_delete(&(*ddr)->openDirCache);
		for (int i = 0; i < (*ddr)->dirDataQueueProcessor->numThreads; i++) {
			int added = qp_add((*ddr)->dirDataQueueProcessor, NULL);
			if (!added) {
				srfsLog(LOG_ERROR, "ddr_delete failed to add NULL to dirDataQueueProcessor\n");
			}
		}
		qp_delete(&(*ddr)->dirDataQueueProcessor);
		try {
			int	i;
			
			for (i = 0; i < DDR_DHT_THREADS; i++) {
				(*ddr)->ansp[i]->waitForActiveOps();
				(*ddr)->ansp[i]->close();
				delete (*ddr)->ansp[i];
				(*ddr)->ansp[i] = NULL;
			}
		} catch (SKRetrievalException & e ){
			srfsLog(LOG_ERROR, "ddr dht batch at %s:%d\n%s\n", __FILE__, __LINE__, e.what());
			srfsLog(LOG_ERROR, " %s\n",  e.getDetailedFailureMessage().c_str());
			fatalError("exception in ddr_delete", __FILE__, __LINE__ );
		} catch (std::exception & ex) {
			srfsLog(LOG_ERROR, "exception in ddr_delete: what: %s\n", ex.what());
			fatalError("exception in ddr_delete", __FILE__, __LINE__ );
		}
		
		if ((*ddr)->pSession) {
			int	i;
			
			for (i = 0; i < DDR_DHT_THREADS; i++) {
				if ((*ddr)->pSession[i]) {
					delete (*ddr)->pSession[i];
					(*ddr)->pSession[i] = NULL;
				}
			}
		}
		
		mem_free((void **)ddr);
	} else {
		fatalError("bad ptr in ddr_delete");
	}
}

// cache

static void ddr_store_DirData_as_OpenDir_in_cache(DirDataReadRequest *ddrr, DirData *dd) {
	CacheStoreResult	result;
	OpenDir	*od;

	srfsLog(LOG_FINE, "Storing od in cache %s", ddrr->path);
	od = od_new(ddrr->path, dd);
	result = odc_store(ddrr->dirDataReader->openDirCache, ddrr->path, od);
	if (result == CACHE_STORE_SUCCESS) {
		srfsLog(LOG_FINE, "Cache store success %s", ddrr->path);
	} else {
		srfsLog(LOG_FINE, "Cache store rejected %s", ddrr->path);
		od_delete(&od);
	}
}

/**
 * Update DirData in the cached OpenDir. (OpenDir must already be created and cached.)
 */
static void ddr_update_DirData_in_cache(DirDataReadRequest *ddrr, DirData *dd, SKMetaData *metaData) {	
	CacheReadResult	result;
	OpenDir			*_od;
	
	srfsLog(LOG_FINE, "ddr_update_DirData_in_cache %llx %s %llx", ddrr, ddrr->path, metaData);
	_od = NULL;
	result = odc_read_no_op_creation(ddrr->dirDataReader->openDirCache, ddrr->path, &_od);
	if (result != CRR_FOUND) {
        if (dd != NULL) {
            srfsLog(LOG_ERROR, "od not found for %s", ddrr->path);
            fatalError("od not found", __FILE__, __LINE__);
        } else {
            // NULL dd is simply an indication to trigger an update if local data is present
            // If no local data is present, we can ignore this
        }
	} else {
		od_add_DirData(_od, dd, metaData);
	}
	srfsLog(LOG_FINE, "out ddr_update_DirData_in_cache %llx %s %llx", ddrr, ddrr->path, metaData);
}


static void ddr_process_dht_batch(void **requests, int numRequests, int curThreadIndex) {
	SKOperationState::SKOperationState   dhtMgetErr;
	DirDataReader	*ddr;
	int				i;
	ActiveOpRef		*refs[numRequests]; // Convenience cast of requests to ActiveOpRef*
	uint64_t		t1;
	uint64_t		t2;
	std::set<string>	seenDirs;
	int					isFirstSeen[numRequests];
	
	srfsLog(LOG_FINE, "in ddr_process_dht_batch %d", curThreadIndex);
	ddr = NULL;
    StrVector       requestGroup;  // list of keys

	// First create a group of keys to request
	for (int i = 0; i < numRequests; i++) {
		ActiveOp		*op;
		DirDataReadRequest	*ddrr;

		refs[i] = (ActiveOpRef *)requests[i];
		op = refs[i]->ao;
		ddrr = (DirDataReadRequest *)ao_get_target(op);
		ddrr_display(ddrr, LOG_FINE);
		if (ddr == NULL) {
			ddr = ddrr->dirDataReader;
		} else {
			if (ddr != ddrr->dirDataReader) {
				fatalError("multi DirDataReader batch");
			}
		}
		srfsLog(LOG_FINE, "looking in dht for dir %s", ddrr->path);
		if (seenDirs.insert(std::string(ddrr->path)).second) {
			requestGroup.push_back(ddrr->path);
			isFirstSeen[i] = TRUE;
		} else {
			isFirstSeen[i] = FALSE;
		}
	}

	// Now fetch the batch from the KVS
	srfsLog(LOG_FINE, "ddr_process_dht_batch calling multi_get");
    SKAsyncValueRetrieval * pValRetrieval = NULL;
	StrSVMap	*pValues = NULL;
    //TODO: Review Ops and Return vals
    srfsLog(LOG_INFO, "got dir nsp %s ", SKFS_DIR_NS );
    try {
	    t1 = curTimeMillis();
	    pValRetrieval = ddr->ansp[curThreadIndex]->get(&requestGroup);
        pValRetrieval->waitForCompletion();
        t2 = curTimeMillis();
        rts_add_sample(ddr->rtsDirData, t2 - t1, numRequests);
        dhtMgetErr = pValRetrieval->getState();
        srfsLog(LOG_FINE, "ddr_process_dht_batch multi_get complete %d", dhtMgetErr);
        pValues = pValRetrieval->getStoredValues();
    } catch (SKRetrievalException & e) {
        srfsLog(LOG_INFO, "ddr line %d SKRetrievalException %s\n", __LINE__, e.what());
		// The operation generated an exception. This is typically simply because
		// values were not found for one or more keys (depending on namespace options.)
		// This could also, however, be caused by a true error.
		dhtMgetErr = SKOperationState::FAILED;
	} catch (SKClientException & e) {
        srfsLog(LOG_WARNING, "ddr line %d SKClientException %s\n", __LINE__, e.what());
		dhtMgetErr = SKOperationState::FAILED;
		// Shouldn't reach here as the only currently thrown exception 
		// is a RetrievalException which is handled above
		fatalError("ddr unexpected SKClientException", __FILE__, __LINE__);
	}

    //srfsLog(LOG_WARNING, "ddr_process_dht_batch got %d values in group %s ", pValues->size(), SKFS_DIR_NS );
    if (!pValues){
        srfsLog(LOG_WARNING, "ddr dhtErr no keys from namespace %s", SKFS_DIR_NS);
        sd_op_failed(ddr->sd, dhtMgetErr);
    } else {
		// Walk through the map and handle each result
        OpStateMap  *opStateMap = pValRetrieval->getOperationStateMap();
        for (i = numRequests - 1; i >= 0; i--) { // in reverse order so that we know when to delete ppval
            ActiveOp		*op;
            DirDataReadRequest	*ddrr;
            int				successful;
            SKStoredValue   *ppval = NULL;
            SKOperationState::SKOperationState  opState;

            successful = FALSE;
            op = refs[i]->ao;
            ddrr = (DirDataReadRequest *)ao_get_target(op);
            try {
                opState = opStateMap->at(ddrr->path);
            } catch(std::exception& emap) { 
                opState = SKOperationState::FAILED;
                srfsLog(LOG_INFO, "ddr std::map exception at %s:%d\n%s\n", __FILE__, __LINE__, emap.what()); 
            }
            if (opState == SKOperationState::SUCCEEDED) {
                try {
                    ppval = pValues->at(ddrr->path);
                } catch(std::exception& emap) { 
                    ppval = NULL;
                    srfsLog(LOG_INFO, "ddr std::map exception at %s:%d\n%s\n", __FILE__, __LINE__, emap.what()); 
                }
                if (ppval == NULL ){
	                srfsLog(LOG_WARNING, "ddr dhtErr no val %s %d line %d", ddrr->path, opState,  __LINE__);
					ddr_update_DirData_in_cache(ddrr, NULL, NULL); // Trigger an update
	            }
            } else if (opState == SKOperationState::INCOMPLETE) {
                sd_op_failed(ddr->sd, dhtMgetErr);
	            srfsLog(LOG_FINE, "%s not found in dht. Incomplete operation state.", ddrr->path);
            } else {  //SKOperationState::FAILED
                SKFailureCause::SKFailureCause cause = SKFailureCause::ERROR;
				try {
					cause = pValRetrieval->getFailureCause();
					if (cause == SKFailureCause::MULTIPLE){
						//sd_op_failed(ddr->sd, dhtMgetErr);
						// non-existence can reach here
						srfsLog(LOG_INFO, "ddr dhtErr %s %d %d %d/%d line %d", ddrr->path, opState, cause, i, numRequests, __LINE__);
					}
					else if (cause != SKFailureCause::NO_SUCH_VALUE) {
						sd_op_failed(ddr->sd, dhtMgetErr);
						srfsLog(LOG_WARNING, "ddr dhtErr %s %d %d %d/%d line %d", ddrr->path, opState, cause, i, numRequests, __LINE__);
					} else { 
						srfsLog(LOG_FINE, "ddr dhtErr %s %d %d %d/%d line %d", ddrr->path, opState, cause, i, numRequests, __LINE__);
						ddr_update_DirData_in_cache(ddrr, NULL, NULL); // Trigger an update
					}
				} catch(SKClientException & e) { 
					srfsLog(LOG_ERROR, "ddr getFailureCause at %s:%d\n%s\n", __FILE__, __LINE__, e.what()); 
					sd_op_failed(ddr->sd, dhtMgetErr);
				} catch(std::exception& e) { 
					srfsLog(LOG_ERROR, "ddr getFailureCause exception at %s:%d\n%s\n", __FILE__, __LINE__, e.what()); 
					sd_op_failed(ddr->sd, dhtMgetErr);
				}
            }
			// If we found a value, store it in the cache
			if (ppval) {
				SKVal	*p;
				
				p = ppval->getValue();
				switch (ddrr->type) {
				case DDRR_Initial:
					ddr_store_DirData_as_OpenDir_in_cache(ddrr, (DirData *)p->m_pVal);
					break;
				case DDRR_Update:
					{
						SKMetaData	*metaData;
						
						metaData = ppval->getMetaData();
						if (metaData) {
							ddr_update_DirData_in_cache(ddrr, (DirData *)p->m_pVal, metaData);
							delete metaData;
						} else {
							srfsLog(LOG_ERROR, "Unexpected no metaData from %llx", p->m_pVal);
						}
					}
					break;
				default:
					fatalError("Panic", __FILE__, __LINE__);
				}
				sk_destroy_val(&p);
				if (isFirstSeen[i]) {
					delete ppval;
				}
			} else {
				// FUTURE - AUTO CREATE IF REQUESTED?
			}

			srfsLog(LOG_FINE, "set op complete %llx", op);
			ao_set_complete(op);
            aor_delete(&refs[i]);
        }
        delete opStateMap; 
        delete pValues;
    }

    pValRetrieval->close();
    delete pValRetrieval;
	srfsLog(LOG_FINE, "out ddr_process_dht_batch");
}

// Callback from Cache. Cache write lock is held during callback.
ActiveOp *ddr_create_active_op(void *_ddr, void *_path, uint64_t noMinModificationTime) {
	DirDataReader *ddr;
	char *path;
	ActiveOp *op;
	DirDataReadRequest	*dirDataReadRequest;

	ddr = (DirDataReader *)_ddr;
	path = (char *)_path;
	srfsLog(LOG_FINE, "ddr_create_active_op %s", path);
	dirDataReadRequest = ddrr_new(ddr, path, DDRR_Initial);
	ddrr_display(dirDataReadRequest, LOG_FINE);
	op = ao_new(dirDataReadRequest, (void (*)(void **))ddrr_delete);
	return op;
}

////////////////////////////////////////////////////////////

DirData *ddr_get_DirData(DirDataReader *ddr, char *path) {
	int	result;
	OpenDir	*od;

	srfsLog(LOG_FINE, "ddr_get_DirData %s", path);
	od = NULL;
	result = ddr_get_OpenDir(ddr, path, &od, DDR_NO_AUTO_CREATE);
	if (result == 0) {
		ddr_check_for_update(ddr, od);
		srfsLog(LOG_FINE, "out1 ddr_get_DirData %s", path);
		return od_get_DirData(od);
	} else {
		srfsLog(LOG_FINE, "out2 ddr_get_DirData %s", path);
		return NULL;
	}
}

void ddr_check_for_update(DirDataReader *ddr, OpenDir *od) {
	if (od_getElapsedSinceLastUpdateMillis(od) > DDR_UPDATE_INTERVAL_MILLIS) {
		ddr_update_OpenDir(ddr, od);
	}
}

void ddr_check_for_reconciliation(DirDataReader *ddr, char *path) {
	OpenDir	*od;
	int		result;

	od = NULL;
	result = ddr_get_OpenDir(ddr, path, &od, DDR_NO_AUTO_CREATE);
	if (result == 0) {
		if (od->needsReconciliation > 0) { // unsafe access, using as a hint
			srfsLog(LOG_INFO, "Reconciliation required %s", path);
			ddr_update_OpenDir(ddr, od);
		} else {
			srfsLog(LOG_INFO, "No reconciliation required %s", path);
		}
	}
}

void ddr_update_OpenDir(DirDataReader *ddr, OpenDir *od) {
	int			result;
	char		*path;
	ActiveOp 	*op;
	ActiveOpRef	*q_aor;
	int			added;
	DirDataReadRequest	*ddrr;
	ActiveOpRef	*aor;
	
	path = od->path;
	srfsLog(LOG_FINE, "ddr_update_OpenDir %s", path);		
	srfsLog(LOG_FINE, "ddr requesting DirData update for %s", path);
	ddrr = ddrr_new(ddr, path, DDRR_Update);
	op = ao_new(ddrr, (void (*)(void **))ddrr_delete);
	q_aor = aor_new(op, __FILE__, __LINE__);
	aor = aor_new(op, __FILE__, __LINE__);
	added = qp_add(ddr->dirDataQueueProcessor, q_aor);
	if (!added) {
		aor_delete(&q_aor);
		// op deletion handled automatically as always
		srfsLog(LOG_FINE, "ddr OpenDir DirData update failed for %s", path);
	} else {
		aor_wait_for_completion(aor);
		srfsLog(LOG_FINE, "ddr OpenDir DirData update complete for %s", path);
	}
	aor_delete(&aor);
	srfsLog(LOG_FINE, "out1 ddr_update_OpenDir %s", path);
}

int ddr_get_OpenDir(DirDataReader *ddr, char *path, OpenDir **od, int createIfNotFound) {
	int		errorCode;

	srfsLog(LOG_FINE, "in ddr_get_OpenDir: %s", path);
	errorCode = _ddr_get_OpenDir(ddr, path, od, createIfNotFound);
	
	if (errorCode != 0) {
		// Found an error. Handle it			
		if (errorCode != ENOENT) {
			// Error is a true error
			// leave errorCode unchanged
		} else {
			// Error is ENOENT
			// leave errorCode unchanged
		}
		if (errorCode != 0 && od != NULL) {
			*od = NULL;
		}
	}
	return errorCode;
}

static int _ddr_get_OpenDir(DirDataReader *ddr, char *path, OpenDir **od, int createIfNotFound) {
	CacheReadResult	result;
	ActiveOpRef		*activeOpRef;
	int				returnCode;
	
	returnCode = 0;
	activeOpRef = NULL;
	
	// Look in the OpenDirCache for an existing operation
	// Create a new operation if none exists

	srfsLog(LOG_FINE, "looking in openDirCache for %s", path);
	result = odc_read(ddr->openDirCache, path, od, &activeOpRef, ddr);
	srfsLog(LOG_FINE, "openDirCache result %d %s", result, crr_strings[result]);
	if (result != CRR_ACTIVE_OP_CREATED) {
		// No operation was created. We must have a result, an existing operation, or an error code to return.
		if (result == CRR_FOUND) {
			// Found in cache, and hence already copied out by the read()
			// We only need to update cache stats here
			//srfsLog(LOG_FINE, "ddr_get_OpenDir inode %lu \n", stbuf->st_ino );
			srfsLog(LOG_FINE, "out ddr_get_OpenDir: %s\tfound in cache", path);
			//rs_cache_inc(ddr->rs);
			return 0;
		} else if (result == CRR_ACTIVE_OP_EXISTING) {
            // Found an existing operation.
			// Copy the data out (done below at end)
			// No fallback to native here since the initial operation will do that
			srfsLog(LOG_FINE, "waiting for existing op completion %s", path);
			aor_wait_for_completion(activeOpRef);
			aor_delete(&activeOpRef);
			//rs_opWait_inc(ddr->rs);
		} else if (result == CRR_ERROR_CODE) {
			int	errorCode;
			
			errorCode = *((int *)od);
			return errorCode;
		} else {
			// Can't legitimately reach here. All cases should have been handled above.
			fatalError("panic", __FILE__, __LINE__);
		}
	} else {
		ActiveOp *op;

		// A new operation was created. 
		
		// First look in the dht
		srfsLog(LOG_FINE, "CRR_ACTIVE_OP_CREATED. Adding to dht queue %s", path);
		op = activeOpRef->ao;

		if (!sd_is_enabled(ddr->sd)) {
			srfsLog(LOG_WARNING, "sd not enabled in DirDataReader");
		}
		
		{
			int			added;
			ActiveOpRef	*aor;

			srfsLog(LOG_FINE, "Queueing op %s %llx", path, op);
			aor = aor_new(op, __FILE__, __LINE__);
			added = qp_add(ddr->dirDataQueueProcessor, aor);
			if (!added) {
				aor_delete(&aor);
			}
		}
		
		srfsLog(LOG_FINE, "Waiting for dht op completion %s", path);
		aor_wait_for_completion(activeOpRef);
		srfsLog(LOG_FINE, "op dht stage complete %s", path);
		result = odc_read_no_op_creation(ddr->openDirCache, path, od);
		srfsLog(LOG_FINE, "cache result %d %s", result, crr_strings[result]);

		// Check to see if the dht had the attr
		if (result == CRR_FOUND) {
			// dht had it. we're done
			//rs_dht_inc(ddr->rs);
			srfsLog(LOG_FINE, "od found in dht");
			aor_delete(&activeOpRef);
			return 0;
		} else if (result == CRR_ACTIVE_OP_EXISTING) {
			// This means that our second check for a result failed.
			// (In this section of code, the "existing operation" was created by this call to ar_get_attr.)
			srfsLog(LOG_FINE, "%s is not native, op not found", path);
			result = CRR_NOT_FOUND;
			odc_remove_active_op(ddr->openDirCache, path);
			//srfsLog(LOG_FINE, "waiting (final) for kvs op completion %s", path);
			aor_delete(&activeOpRef);
		} else if (result == CRR_ERROR_CODE) {
			int	errorCode;

			errorCode = *((int *)od);
			return errorCode;
		} else {
			fatalError("panic", __FILE__, __LINE__);
			// shouldn't reach here since we should either find the
			// result or find the active op
		}
	}

	srfsLog(LOG_FINE, "result %d %s %s %d\n", result, path, __FILE__, __LINE__);
	// below will copy the data out if it exists
	result = odc_read_no_op_creation(ddr->openDirCache, path, od);
	srfsLog(LOG_FINE, "cache result %d %s", result, crr_strings[result]);
	switch (result) {
	case CRR_FOUND:
		returnCode = 0;
		break;
	case CRR_ERROR_CODE:
		returnCode = *((int *)od);
		break;
	case CRR_ACTIVE_OP_EXISTING:
        // We only get here if we didn't create the operation, but the data wasn't found
        // In this case, we fall through to the not found case
	case CRR_NOT_FOUND:
		returnCode = ENOENT;
		if (createIfNotFound == DDR_AUTO_CREATE) {
			CacheStoreResult	result;
			
			*od = od_new(path, NULL);
			result = odc_store(ddr->openDirCache, path, *od);
			if (result == CACHE_STORE_SUCCESS) {
				returnCode = 0;
			} else {
                CacheReadResult crr;
                
                // must have been stored by another thread
                // delete the one created here and read from the cache
                od_delete(od);
                crr = odc_read_no_op_creation(ddr->openDirCache, path, od);
                if (crr != CRR_FOUND) {
                    // Shouldn't happen since we only get here if the store was rejected
                    fatalError("odc_read_no_op_creation after odc store failed", __FILE__, __LINE__);
                } else {
                    returnCode = 0;
                }
			}
		}
		break;
	default: 
		srfsLog(LOG_WARNING, "unexpected result %d", result);
		srfsLog(LOG_WARNING, "crr_strings -> %s", crr_strings[result]);
		fatalError("panic", __FILE__, __LINE__);
	}
	if (activeOpRef != NULL) {
		srfsLog(LOG_FINE, "Deleting ref %s", path);
		aor_delete(&activeOpRef);
	}
	if (returnCode != 0 && returnCode != ENOENT) {
		srfsLog(LOG_WARNING, "path %s returnCode %d line %d\n", path, returnCode, __LINE__);
	}
	srfsLog(LOG_FINE, "out _ddr_get_OpenDir: %s\top completed. returnCode %d", path, returnCode);
	return returnCode;
}

void ddr_display_stats(DirDataReader *ddr, int detailedStats) {
	srfsLog(LOG_WARNING, "DirDataReader Stats");
	//rs_display(ddr->rs);
	if (detailedStats) {
		odc_display_stats(ddr->openDirCache);
	}
	srfsLog(LOG_WARNING, "ddr ResponseTimeStats: DHT");
	rts_display(ddr->rtsDirData);
}