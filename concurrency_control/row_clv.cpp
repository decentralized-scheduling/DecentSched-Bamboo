#include "row.h"
#include "txn.h"
#include "row_clv.h"
#include "mem_alloc.h"
#include "manager.h"

void Row_clv::init(row_t * row) {
	_row = row;
	// owners is a single linked list, each entry/node contains info like lock type, prev/next
	owners = NULL;
	owners_tail = NULL;
	// waiter is a double linked list. two ptrs to the linked lists
	waiters_head = NULL;
	waiters_tail = NULL;
	// retired is a linked list, the next of tail is the head of owners
	retired_head = NULL;
	retired_tail = NULL;
	owner_cnt = 0;
	waiter_cnt = 0;
	retired_cnt = 0;
	// local timestamp
	local_ts = -1;

	latch = new pthread_mutex_t;
	pthread_mutex_init(latch, NULL);

	blatch = false;
}

RC Row_clv::lock_get(lock_t type, txn_man * txn) {
	uint64_t *txnids = NULL;
	int txncnt = 0;
	return lock_get(type, txn, txnids, txncnt);
}

RC Row_clv::lock_get(lock_t type, txn_man * txn, uint64_t* &txnids, int &txncnt) {
	assert (CC_ALG == CLV);
	CLVLockEntry * en;

	if (g_central_man)
		glob_manager->lock_row(_row);
	else 
		pthread_mutex_lock( latch );

	// each thread has at most one owner of a lock
	assert(owner_cnt <= g_thread_cnt);
	// each thread has at most one waiter
	assert(waiter_cnt < g_thread_cnt);

	// 1. set txn to abort in owners and retired

	RC rc = WAIT;
	RC status = RCOK;
	bool unassigned = false;

	// if unassigned, assign the largest possible number
	local_ts = -1;
	if ( txn->get_ts() == 0 && 
		( (waiter_cnt != 0) || (retired_cnt != 0) || (owner_cnt != 0 && conflict_lock(owners->type, type)) ) ) {
		local_ts = txn->set_next_ts(retired_cnt + owner_cnt + 1);
		// if == 0, fail to assign, oops, self has an assigned number anyway
		// if != 0, already booked n ts. 
		if (local_ts != 0) {
			unassigned = true;
			local_ts = local_ts - (retired_cnt + owner_cnt);
			assert(txn->get_ts() != local_ts); // make sure did not change pointed addr
		}
	}

	// check retired
	status = wound_conflict(type, txn, txn->get_ts(), retired_head, status, unassigned);
	if (status == Abort) {
		rc = Abort;
		bring_next();
		goto final;
	}

	// check owners
	status = wound_conflict(type, txn, txn->get_ts(), owners, status, unassigned);
	if (status == Abort) {
		rc = Abort;
		bring_next();
		goto final;
	}

	// 2. insert into waiters and bring in next waiter
	insert_to_waiters(type, txn);
	bring_next();

#if DEBUG_ASSERT
	if (waiter_cnt != 0) {
		assert(owner_cnt != 0);
	}
#endif

	// 3. if brought txn in owner, return acquired lock
	en = owners;
	while(en){
		if (en->txn == txn) {
			rc = RCOK;
			break;
		}
		en = en->next;
	}

final:
	if (g_central_man)
		glob_manager->release_row(_row);
	else
		pthread_mutex_unlock( latch );

	return rc;
}

RC Row_clv::lock_retire(txn_man * txn) {

	if (g_central_man)
		glob_manager->lock_row(_row);
	else
		pthread_mutex_lock( latch );

	RC rc = RCOK;

	// 1. find entry in owner and remove
	CLVLockEntry * entry = rm_if_in_owners(txn);
	if (entry == NULL) {
		// may be is aborted
		assert(txn->status == ABORTED);
		rc = Abort;
	}

	// 2. if txn not aborted, try to add to retired
	if (rc != Abort) {
		// 2.1 must clean out retired list before inserting!!
		clean_aborted_retired();
#if DEBUG_ASSERT
		debug();
#endif
		// 2.2 increment barriers if conflicts with tail
		if (retired_tail) {
			if (conflict_lock(retired_tail->type, entry->type)) {
				// default is_cohead = false
				entry->delta = true;
				txn->increment_commit_barriers();
			} else { 
				entry->is_cohead = retired_tail->is_cohead;
				if (!entry->is_cohead)
					txn->increment_commit_barriers();
			}
#if DEBUG_TMP
		printf("[row_clv-%lu txn-%lu (%lu)] retired_tail(%lu)->type %d, entry->type %d, entry->delta %d\n", _row->get_row_id(), txn->get_txn_id(), txn->get_ts(), 
				retired_tail->txn->get_txn_id(), retired_tail->type, entry->type, entry->delta);
#endif
		// 2.3 append entry to retired
		} else {
			entry->is_cohead = true;
		}
		RETIRED_LIST_PUT_TAIL(retired_head, retired_tail, entry);
		retired_cnt++;

	#if DEBUG_CLV
		printf("[row_clv-%lu txn-%lu (%lu)] move to retired (type %d), is_cohead=%d, delta=%d\n",
				_row->get_row_id(), txn->get_txn_id(), txn->get_ts(), entry->type, entry->is_cohead, entry->delta);
	#endif
	#if DEBUG_ASSERT
		debug();
		assert_in_list(retired_head, retired_tail, retired_cnt, txn);
	#endif
	}

	// bring next owners from waiters
	bring_next();

	if (g_central_man)
		glob_manager->release_row(_row);
	else
		pthread_mutex_unlock( latch );

	return rc;
}

RC Row_clv::lock_release(txn_man * txn, RC rc) {
	if (g_central_man)
		glob_manager->lock_row(_row);
	else 
		pthread_mutex_lock( latch );

	CLVLockEntry * en;
	
	// Try to find the entry in the retired
	if (!rm_if_in_retired(txn, rc == Abort)) {
		// Try to find the entry in the owners
		en = rm_if_in_owners(txn);
		if (en) {
			return_entry(en);
		} else {
			if(!rm_if_in_waiters(txn)) {
#if DEBUG_CLV
				printf("[row_clv-%lu txn-%lu (%lu)] cannot find entry when trying to release\n", _row->get_row_id(), txn->get_txn_id(), txn->get_ts()); 
#endif
			}	
		}
	}
	#if DEBUG_ASSERT
	debug();
	assert_notin_list(waiters_head, waiters_tail, waiter_cnt, txn);
	assert_notin_list(retired_head, retired_tail, retired_cnt, txn);
	#endif

	// WAIT - done releasing with is_abort = true
	// FINISH - done releasing with is_abort = false
	bring_next();

	if (g_central_man)
		glob_manager->release_row(_row);
	else
		pthread_mutex_unlock( latch );

	return RCOK;
}


void
Row_clv::clean_aborted_retired() {
	CLVLockEntry * en = retired_head;
	while(en) {
		if (en->txn->lock_abort) {
			en = remove_descendants(en);
		} else {
			en = en->next;
		}
	}
	#if DEBUG_ASSERT
	debug();
	#endif
}

void 
Row_clv::clean_aborted_owner() {
	CLVLockEntry * en = owners;
	CLVLockEntry * prev = NULL;
	while (en) {
		if (en->txn->lock_abort) {
			// no changes to prev
			en = rm_from_owners(en, prev);
		} else {
			prev = en;
			en = en->next;
		}
	}
	#if DEBUG_ASSERT
	debug();
	#endif
}

CLVLockEntry * 
Row_clv::rm_if_in_owners(txn_man * txn) {
	// NOTE: will not destroy entry
	CLVLockEntry * en = owners;
	CLVLockEntry * prev = NULL;
	while (en) {
		if (en->txn == txn)
			break;
		prev = en;
		en = en->next;
	}
	if (en) {
		rm_from_owners(en, prev, false);
		#if DEBUG_ASSERT
		debug();
		#endif
	}
	return en;
}

bool
Row_clv::rm_if_in_retired(txn_man * txn, bool is_abort) {
	CLVLockEntry * en = retired_head;
	while(en) {
		if (en->txn == txn) {
			if (is_abort) {
				en = remove_descendants(en);
			} else {
				assert(txn->status == COMMITED);
				en = rm_from_retired(en);
			}
			#if DEBUG_ASSERT
			debug();
			assert_notin_list(retired_head, retired_tail, retired_cnt, txn);
			#endif
			return true;
		} else {
			en = en->next;
		}
	}
	#if DEBUG_ASSERT
	debug();
	assert_notin_list(retired_head, retired_tail, retired_cnt, txn);
	#endif
	return false;
}

bool 
Row_clv::rm_if_in_waiters(txn_man * txn) {
	CLVLockEntry * en = waiters_head;
	while(en) {
		if (en->txn == txn) {
			LIST_RM(waiters_head, waiters_tail, en, waiter_cnt);
			#if DEBUG_CLV
			printf("[row_clv-%lu txn-%lu (%lu)] rm from waiters\n", 
				_row->get_row_id(), en->txn->get_txn_id(), en->txn->get_ts());
			#endif
			return_entry(en);
			#if DEBUG_ASSERT
			debug();
			assert_notin_list(waiters_head, waiters_tail, waiter_cnt, txn);
			#endif
			return true;
		}
		en = en->next;
	}
	#if DEBUG_ASSERT
	debug();
	assert_notin_list(waiters_head, waiters_tail, waiter_cnt, txn);
	#endif
	return false;
}


CLVLockEntry * 
Row_clv::rm_from_owners(CLVLockEntry * en, CLVLockEntry * prev, bool destroy) {
	CLVLockEntry * to_return = en->next;
	QUEUE_RM(owners, owners_tail, prev, en, owner_cnt);
	if (destroy) {
		// return next entry
		return_entry(en);
	}
	#if DEBUG_CLV
	printf("[row_clv-%lu txn-%lu (%lu)] rm from owners\n", 
		_row->get_row_id(), en->txn->get_txn_id(), en->txn->get_ts());
	#endif
	#if DEBUG_ASSERT
	debug();
	#endif
	// return removed entry
	return to_return;
}

CLVLockEntry * 
Row_clv::rm_from_retired(CLVLockEntry * en) {
	CLVLockEntry * to_return = en->next;
	update_entry(en);
	LIST_RM(retired_head, retired_tail, en, retired_cnt);
	#if DEBUG_CLV
	printf("[row_clv-%lu txn-%lu (%lu)] rm from retired\n", 
		_row->get_row_id(), en->txn->get_txn_id(), en->txn->get_ts());
	#endif
	return_entry(en);
	#if DEBUG_ASSERT
	debug();
	assert_notin_list(retired_head, retired_tail, retired_cnt, en->txn);
	#endif
	return to_return;
}

void
Row_clv::bring_next() {

	clean_aborted_retired();
	clean_aborted_owner();

	CLVLockEntry * entry;
	// If any waiter can join the owners, just do it!
	while (waiters_head) {
		if ((owners == NULL) || (!conflict_lock(owners->type, waiters_head->type))) {
			LIST_GET_HEAD(waiters_head, waiters_tail, entry);
			waiter_cnt --;

			if (entry->txn->lock_abort) {
				continue;
			}
			
			// add to onwers
			QUEUE_PUSH(owners, owners_tail, entry);

			owner_cnt ++;
			ASSERT(entry->txn->lock_ready == 0);
			entry->txn->lock_ready = true;

			#if DEBUG_CLV
			printf("[row_clv-%lu txn-%lu (%lu)] move to owners\n",
					 _row->get_row_id(), entry->txn->get_txn_id(), entry->txn->get_ts());
			#endif
		} else
			break;
	}
	ASSERT((owners == NULL) == (owner_cnt == 0));
}


bool Row_clv::conflict_lock(lock_t l1, lock_t l2) {
	if (l1 == LOCK_NONE || l2 == LOCK_NONE)
		return false;
		else if (l1 == LOCK_EX || l2 == LOCK_EX)
			return true;
	else
		return false;
}

bool Row_clv::conflict_lock_entry(CLVLockEntry * l1, CLVLockEntry * l2) {
	if (l1 == NULL || l2 == NULL)
		return false;
	return conflict_lock(l1->type, l2->type);
}


CLVLockEntry * Row_clv::get_entry() {
	CLVLockEntry * entry = (CLVLockEntry *) mem_allocator.alloc(sizeof(CLVLockEntry), _row->get_part_id());
	entry->prev = NULL;
	entry->next = NULL;
	entry->delta = false;
	entry->is_cohead = false;
	entry->txn = NULL;
	return entry;
}

void Row_clv::return_entry(CLVLockEntry * entry) {
	mem_allocator.free(entry, sizeof(CLVLockEntry));
}


RC
Row_clv::wound_txn(txn_man * txn, CLVLockEntry * en) {
	if (txn->wound_txn(en->txn) == ERROR) {
		#if DEBUG_CLV
		printf("[row_clv-%lu txn-%lu (%lu)] detected aborted when "
		"trying to wound others\n", _row->get_row_id(), txn->get_txn_id(),  txn->get_ts());
		#endif
		return Abort;
	}
	#if DEBUG_CLV
	printf("[row_clv-%lu txn-%lu (%lu)] wound txn %lu (%lu)\n", 
		_row->get_row_id(), txn->get_txn_id(),  txn->get_ts(), en->txn->get_txn_id(), en->txn->get_ts());
	#endif
	return RCOK;
}


RC
Row_clv::wound_conflict(lock_t type, txn_man * txn, ts_t ts, CLVLockEntry * list, RC status, bool unassigned) {
	CLVLockEntry * en = list;
	bool recheck = false;
	while (en != NULL) {
		recheck = false;
		if (en->txn->lock_abort)
			continue;
		if (status == RCOK && conflict_lock(en->type, type) && 
			(txn->get_ts() == 0 || en->txn->get_ts() > ts) ) {
			status = WAIT; // has conflicts
		}
		if (status == WAIT) {
			if (ts != 0) {
				// abort txn
				wound_txn(txn, en);
			}
			if (unassigned) {
				// assign it a ts first
				if (!en->txn->atomic_set_ts(local_ts)) {
					// it has a ts already
					recheck = true;
#if DEBUG_TMP
					printf("txn %lu rechecking\n", txn->get_txn_id());
#endif
				} else {
					local_ts++;
				}
			}
		}
		if (!recheck)
			en = en->next;
	}
	return status;
}

void
Row_clv::insert_to_waiters(lock_t type, txn_man * txn) {
	CLVLockEntry * entry = get_entry();
	entry->txn = txn;
	entry->type = type;
	CLVLockEntry * en = waiters_head;
	while (en != NULL)
	{
		if (txn->get_ts() < en->txn->get_ts())
			break;
		en = en->next;
	}
	if (en) {
		LIST_INSERT_BEFORE(en, entry);
		if (en == waiters_head)
			waiters_head = entry;
	} else {
		LIST_PUT_TAIL(waiters_head, waiters_tail, entry);
	}
	waiter_cnt ++;
	txn->lock_ready = false;

#if DEBUG_CLV
	printf("[row_clv-%lu txn-%lu (%lu)] add to waiters (type %d)\n",
			_row->get_row_id(), txn->get_txn_id(), txn->get_ts(), type);
#endif
#if DEBUG_ASSERT
	assert_in_list(waiters_head, waiters_tail, waiter_cnt, txn);
#endif
}


CLVLockEntry * 
Row_clv::remove_descendants(CLVLockEntry * en) {
	assert(en != NULL);
	CLVLockEntry * to_destroy = NULL;
	CLVLockEntry * prev = en->prev;
	// 1. remove self, set iterator to next entry
	lock_t type = en->type;
	bool conflict_with_owners = conflict_lock_entry(en, owners);
	en = rm_from_retired(en);

	// 2. remove next conflict till end
	// 2.1 find next conflict
	while(en && (!conflict_lock(type, en->type))) {
		en = en->next;
	}
	// 2.2 remove dependees
	if (en == NULL) {
		if (!conflict_with_owners) {
			// clean owners
			while(owners) {
				en = owners;
				en->txn->set_abort();
				#if DEBUG_CLV
				printf("[row_clv-%lu txn-%lu (%lu)] rm descendants from owners\n", 
					_row->get_row_id(), en->txn->get_txn_id(), en->txn->get_ts());
				#endif
				// no need to be too complicated (i.e. call function) as the owner will be empty in the end
				owners = owners->next;
				return_entry(en);
			}
			owners_tail = NULL;
			owners = NULL;
			owner_cnt = 0;
		} // else, nothing to do
	} else {
		// abort till end
		LIST_RM_SINCE(retired_head, retired_tail, en);
		while(en) {
			to_destroy = en;
			#if DEBUG_CLV
			printf("[row_clv-%lu txn-%lu (%lu)] rm descendants from retired\n", 
				_row->get_row_id(), en->txn->get_txn_id(), en->txn->get_ts());
			#endif
			en->txn->set_abort();
			retired_cnt--;
			en = en->next;
			return_entry(to_destroy);
		}
	}
#if DEBUG_ASSERT
	debug();
#endif
	if (prev)
		return prev->next;
	else
		return retired_head;
}


void
Row_clv::update_entry(CLVLockEntry * en) {
	CLVLockEntry * entry;
	if (en->prev) {
		if (en->next) {
			if (en->delta && !en->next->delta) // WR(1)R(0)
				en->next->delta = true;
#if DEBUG_CLV
				printf("[row_clv-%lu txn-%lu (%lu)] change delta=%d is_cohead=%d\n", 
						_row->get_row_id(), en->txn->get_txn_id(), en->txn->get_ts(), en->delta, en->is_cohead);
#endif
		} else {
			// has no next, nothing needs to be updated
		}
	} else {
		// has no previous, en = head
		if (en->next) {
			#if DEBUG_ASSERT
			assert(en == retired_head);
			#endif
			// has next entry
			// en->next->is_cohead = true;
			if (en->next->delta) {
				en->next->delta = false;
				assert(!en->next->is_cohead);
				entry = en->next;
				while(entry && (!entry->delta)) {
					assert(!entry->is_cohead);
					entry->is_cohead = true;
#if DEBUG_CLV
				printf("[row_clv-%lu txn-%lu (%lu)] change delta=%d is_cohead=%d\n", 
						_row->get_row_id(), entry->txn->get_txn_id(), en->txn->get_ts(), entry->delta, entry->is_cohead);
#endif
					entry->txn->decrement_commit_barriers();
					entry = entry->next;
				}
			} // else (R)RR, no changes
		} else {
			// has no next entry, never mind
		}
	}
	assert(retired_head || retired_head->is_cohead);
#if DEBUG_ASSERT
	debug();
#endif
}

/* debug methods */

void
Row_clv::debug() {
	CLVLockEntry * en;
	CLVLockEntry * prev = NULL;
	UInt32 cnt = 0;
	// check retired
	//bool has_conflicts = false;
	en = retired_head;
	while(en) {
		if (cnt == 0) {
			assert(en->is_cohead);
			assert(!en->delta);
		}
		assert(prev == en->prev);
		/*
		if (conflict_lock_entry(prev, en)) {
			assert(en->delta);
			has_conflicts = true;
		}
		if (en != retired_head) {
			if (!conflict_lock_entry(retired_head, en)) {
				if (!has_conflicts) {
					assert(en->is_cohead);
				} else {
					assert(!(en->is_cohead));
				}
			} else {
				assert(!(en->is_cohead));
			}
		} else {
			assert(en->is_cohead);
			assert(!en->delta);
		}
		*/
		cnt += 1;
		prev = en;
		en = en->next;
	}
	assert(prev == retired_tail);
	assert(cnt == retired_cnt);
	// check waiters
	cnt = 0;
	prev = NULL;
	en = waiters_head;
	while(en) {
		assert(prev == en->prev);
		cnt += 1;
		prev = en;
		en = en->next;
	}
	assert(prev == waiters_tail);
	assert(cnt == waiter_cnt);
	// check owner
	cnt = 0;
	prev = NULL;
	en = owners;
	while(en) {
		cnt += 1;
		prev = en;
		en = en->next;
	}
	assert(prev == owners_tail);
	assert(cnt == owner_cnt);
}

void
Row_clv::print_list(CLVLockEntry * list, CLVLockEntry * tail, int cnt) {
	CLVLockEntry * en = list;
	int count = 0;
	while(en){
		printf("(%lu, %d) -> ", en->txn->get_txn_id(), en->type);
		en = en->next;
		count += 1;
	}
	if (tail) {
		printf("expected cnt: %d, real cnt: %d, expected tail: %lu\n", cnt, count, 
		tail->txn->get_txn_id());
	} else {
		printf("expected cnt: %d, real cnt: %d, expected tail is null\n", cnt, count);
	}
}


void
Row_clv::assert_notin_list(CLVLockEntry * list, CLVLockEntry * tail, int cnt, txn_man * txn) {
	CLVLockEntry * en = list;
	CLVLockEntry * prev = NULL;
	int count = 0;
	while(en){
		if(txn->get_txn_id() == en->txn->get_txn_id())
			printf("ERROR: %lu is already in row %lu\n", txn->get_txn_id(), _row->get_row_id());
		assert(txn->get_txn_id() != en->txn->get_txn_id());
		prev = en;
		en = en->next;
		count += 1;
	}
	if (count != cnt){
		print_list(list, tail, cnt);
	}
	assert(count == cnt);
	assert(tail == prev);
}

void
Row_clv::assert_in_list(CLVLockEntry * list, CLVLockEntry * tail, int cnt, txn_man * txn) {
	CLVLockEntry * en = list;
	CLVLockEntry * prev = NULL;
	int count = 0;
	bool in = false;
	while(en){
		if(txn->get_txn_id() == en->txn->get_txn_id()) {
			if (in) {
				print_list(owners, owners_tail, owner_cnt);
				assert(false);
			}
			in = true;
		}
		prev = en;
		en = en->next;
		count += 1;
	}
	#if DEBUG_CLV
	if (tail != prev)
		print_list(list, tail, cnt);
	#endif
	// assert(tail->txn->get_txn_id() == txn->get_txn_id());
	assert(in);
	assert(tail == prev);
	assert(count == cnt);
}

void
Row_clv::assert_in_list(CLVLockEntry * list, CLVLockEntry * tail, int cnt,CLVLockEntry * l) {
	CLVLockEntry * en = list;
	CLVLockEntry * prev = NULL;
	txn_man * txn = l->txn;
	int count = 0;
	bool in = false;
	while(en){
		if(txn->get_txn_id() == en->txn->get_txn_id()) {
			if (in) {
				print_list(owners, owners_tail, owner_cnt);
				assert(false);
			}
			in = true;
		}
		prev = en;
		en = en->next;
		count += 1;
	}
	#if DEBUG_CLV
	if (tail != prev)
		print_list(list, tail, cnt);
	#endif
	// assert(tail->txn->get_txn_id() == txn->get_txn_id());
	assert(in);
	assert(tail == prev);
	assert(count == cnt);
}

bool
Row_clv::has_conflicts_in_list(CLVLockEntry * list, CLVLockEntry * entry) {
	CLVLockEntry * en;
	en = list;
	while(en) {
		if (conflict_lock(en->type, entry->type)) {
			return true;
		}
		en = en->next;
	}
	return false;
}

