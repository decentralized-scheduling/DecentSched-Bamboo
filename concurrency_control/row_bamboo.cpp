#include "row.h"
#include "txn.h"
#include "row_bamboo.h"
#include "mem_alloc.h"
#include "manager.h"

void Row_bamboo::init(row_t * row) {
  Row_bamboo_pt::init(row);
}

RC Row_bamboo::lock_get(lock_t type, txn_man * txn, uint64_t* &txnids, int
&txncnt, Access * access) {
  // allocate an lock entry
  ASSERT(CC_ALG == BAMBOO);
  BBLockEntry * to_insert = get_entry(access);
  to_insert->type = type;
  BBLockEntry * en = NULL;
  RC rc = RCOK;
  ts_t owner_ts = 0;

#if DEBUG_ABORT_LENGTH
  txn->abort_chain = 0;
#endif
#if DEBUG_CS_PROFILING
  uint64_t starttime = get_sys_clock();
#endif
  lock(to_insert);
#if DEBUG_CS_PROFILING
  INC_STATS(txn->get_thd_id(), time_get_latch, get_sys_clock() - starttime);
  starttime = get_sys_clock();
#endif

  ts_t ts = txn->get_ts();
  bool retired_has_write = (retired_tail && (retired_tail->type==LOCK_EX ||
      !retired_tail->is_cohead));
  if (type == LOCK_SH) {
    if (!retired_has_write) {
      if (!owners) {
        // append to retired
        ADD_TO_RETIRED_TAIL(to_insert);
      } else {
        // has write in owner but not in retired
        owner_ts = owners->txn->get_ts();
        // the only writer in owner may be unassigned
        if (owner_ts == 0) {
            // assign owner a ts
            owner_ts = owners->txn->set_next_ts(1);
            // if fail to assign owner a ts, recheck
            if (owner_ts == 0)
              owner_ts = owners->txn->get_ts(); 
        }
        if (ts == 0) {
          ts = txn->set_next_ts(1);
          // if fail to assign, reload
          if ( ts == 0 )
            ts = txn->get_ts();
        }
        if (ts > owner_ts) {
            ADD_TO_WAITERS(en, to_insert);
          goto final;
        } else {
#if BB_OPT_RAW
          access->data->copy(owners->access->orig_data);
          ADD_TO_RETIRED_TAIL(to_insert);
          rc = FINISH;
#else
          assert(false);
#endif
        }
      }
    } else {
      // special case: [W][]
      if (retired_cnt == 1 && (retired_head->type == LOCK_EX))
         retired_head->txn->set_next_ts(1);
      // retire has write -> retire all has ts. owner has ts if exists.
      if (ts == 0) {
        ts = txn->set_next_ts(1); // 1 for owner, 1 for self
        // if fail to assign, reload
        if ( ts == 0 )
          ts = txn->get_ts();
      }
      if (!owners || (owners->txn->get_ts() > ts)) {
#if BB_OPT_RAW
        // go through retired and insert
        en = retired_head;
        while (en) {
          if (en->type == LOCK_EX && (en->txn->get_ts() > ts))
            break;
          en = en->next;
        }
        if (en) {
          access->data->copy(en->access->orig_data);
          INSERT_TO_RETIRED(to_insert, en);
        } else {
          if (owners)
            access->data->copy(owners->access->orig_data);
          ADD_TO_RETIRED_TAIL(to_insert);
        }
        rc = FINISH;
#else
        assert(false);
#endif
      } else {
        ADD_TO_WAITERS(en, to_insert);
        goto final;
      }
    }
  } else {
    // LOCK_EX
    if (retired_cnt == 0 && !owners) {
      // grab directly, no ts needed
      owners = to_insert;
      owners->status = LOCK_OWNER;
      txn->lock_ready = true;
      goto final;
    }
    if (waiter_cnt > BB_OPT_MAX_WAITER && (BB_OPT_MAX_WAITER != 0)) {
      rc = Abort;
      goto final;
    }
    if (owners)
      owner_ts = owners->txn->get_ts();
    // has to assign self ts if not have one
    if (ts == 0) {
      if (retired_has_write || owners) {
        // false-true: [  ][W] -- owner may be unassigned
        // true-false: [W][ ] -- special case, otherwise all assigned
        // true-true:  [WR][W] -- all assigned
        if (retired_has_write && (retired_cnt == 1) && retired_head->type == LOCK_EX)
          retired_head->txn->set_next_ts(1);
        // assign owner
        if (owners && (owner_ts == 0)) {
          owner_ts = owners->txn->set_next_ts(1);
          if (owner_ts == 0)
            owner_ts = owners->txn->get_ts();
        }
        ts = txn->set_next_ts(1); // 1 for owner, 1 for self
        if (ts == 0) {
          ts = txn->get_ts();
        }
      } else { // owner is empty, retired may have unassigned reads
        // retired_has_write = false & owners = NULL
        // assgin each retired, then assign self and add to OWNER
        // as no waiters for sure
        en = retired_head;
        while (en) {
          en->txn->set_next_ts(1);
          en = en->next;
        }
        ts = txn->set_next_ts(1);
        if (ts != 0) {
          owners = to_insert;
          owners->status = LOCK_OWNER;
          txn->lock_ready = true;
          goto final;
        } else {
          ts = txn->get_ts(); // assigned by others already, need to continue
        }
      }
    } 
    // else: self has timestamp already. 
    // then need to kill all unassigned entries
    // if ts == 0, all timestamps should be assigned already!
    if (!owners || (owner_ts > ts || (owner_ts == 0))) {
      // go through retired and wound
      en = retired_head;
      while (en) {
        owner_ts = en->txn->get_ts(); // just use this var, but actually retired_ts
        if (owner_ts == 0 || (owner_ts > ts)) {
          TRY_WOUND_PT(en, to_insert);
          // abort descendants
          en = rm_from_retired(en, true, txn);
        } else
          en = en->next;
      }
      if (owners) {
        // abort owners as well
        TRY_WOUND_PT(owners, to_insert);
        return_entry(owners);
        owners = NULL;
      }
    }
    ADD_TO_WAITERS(en, to_insert);
    if (bring_next(txn)) {
      rc = RCOK;
    } else {
      goto final;
    }
  }
  txn->lock_ready = true;

  final:
#if DEBUG_CS_PROFILING
  INC_STATS(txn->get_thd_id(), time_get_cs, get_sys_clock() - starttime);
#endif
  unlock(to_insert);
#if DEBUG_ABORT_LENGTH
  if (txn->abort_chain > 0)
    UPDATE_STATS(txn->get_thd_id(), abort_length, txn->abort_chain);
#endif
  return rc;
}