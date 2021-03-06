// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef __PG_H
#define __PG_H


#include "include/types.h"
#include "osd_types.h"
#include "include/buffer.h"

#include "OSDMap.h"
#include "ObjectStore.h"
#include "msg/Messenger.h"

#include "common/DecayCounter.h"

#include <list>
#include <string>
using namespace std;

#include <ext/hash_map>
#include <ext/hash_set>
using namespace __gnu_cxx;


class OSD;
class MOSDOp;
class MOSDOpReply;
class MOSDSubOp;
class MOSDSubOpReply;
class MOSDPGActivateSet;

/** PG - Replica Placement Group
 *
 */

class PG {
public:
  
  /*
   * PG::Info - summary of PG statistics.
   *
   * some notes: 
   *  - last_complete implies we have all objects that existed as of that
   *    stamp, OR a newer object, OR have already applied a later delete.
   *  - if last_complete >= log.bottom, then we know pg contents thru log.top.
   *    otherwise, we have no idea what the pg is supposed to contain.
   */
  struct Info {
    pg_t pgid;
    eversion_t last_update;    // last object version applied to store.
    eversion_t last_complete;  // last version pg was complete through.

    eversion_t log_bottom;     // oldest log entry.
    bool       log_backlog;    // do we store a complete log?

    epoch_t last_epoch_started;  // last epoch started.
    epoch_t last_epoch_finished; // last epoch finished.

    struct History {
      epoch_t same_since;          // same acting set since
      epoch_t same_primary_since;  // same primary at least back through this epoch.
      epoch_t same_acker_since;    // same acker at least back through this epoch.
      History() : same_since(0), same_primary_since(0), same_acker_since(0) {}
    } history;
    
    Info(pg_t p=0) : pgid(p), 
                     log_backlog(false),
                     last_epoch_started(0), last_epoch_finished(0) {}
    bool is_uptodate() const { return last_update == last_complete; }
    bool is_empty() const { return last_update.version == 0; }
  };
  
  
  /** 
   * Query - used to ask a peer for information about a pg.
   *
   * note: if version=0, type=LOG, then we just provide our full log.
   *   only if type=BACKLOG do we generate a backlog and provide that too.
   */
  struct Query {
    const static int INFO = 0;
    const static int LOG = 1;
    const static int BACKLOG = 2;
    const static int FULLLOG = 3;

    int type;
    eversion_t split, floor;
    Info::History history;

    Query() : type(-1) {}
    Query(int t, Info::History& h) : 
      type(t), history(h) { assert(t != LOG); }
    Query(int t, eversion_t s, eversion_t f, Info::History& h) : 
      type(t), split(s), floor(f), history(h) { assert(t == LOG); }
  };
  
  
  /*
   * Missing - summary of missing objects.
   *  kept in memory, as a supplement to Log.
   *  also used to pass missing info in messages.
   */
  class Missing {
  public:
    map<object_t, eversion_t> missing;   // oid -> v
    map<eversion_t, object_t> rmissing;  // v -> oid

    map<object_t, int>       loc;       // where i think i can get them.

    int num_lost() const { return missing.size() - loc.size(); }
    int num_missing() const { return missing.size(); }

    bool is_missing(object_t oid) {
      return missing.count(oid);
    }
    bool is_missing(object_t oid, eversion_t v) {
      return missing.count(oid) && missing[oid] <= v;
    }
    void add(object_t oid) {
      eversion_t z;
      add(oid,z);
    }
    void add(object_t oid, eversion_t v) {
      if (missing.count(oid)) {
        if (missing[oid] > v) return;   // already missing newer.
        rmissing.erase(missing[oid]);
      }
      missing[oid] = v;
      rmissing[v] = oid;
    }
    void rm(object_t oid, eversion_t when) {
      if (missing.count(oid) && missing[oid] < when) {
        rmissing.erase(missing[oid]);
        missing.erase(oid);
        loc.erase(oid);
      }        
    }
    void got(object_t oid, eversion_t v) {
      assert(missing.count(oid));
      assert(missing[oid] <= v);
      loc.erase(oid);
      rmissing.erase(missing[oid]);
      missing.erase(oid);
    }
    void got(object_t oid) {
      assert(missing.count(oid));
      loc.erase(oid);
      rmissing.erase(missing[oid]);
      missing.erase(oid);
    }

    void _encode(bufferlist& blist) {
      ::_encode(missing, blist);
      ::_encode(loc, blist);
    }
    void _decode(bufferlist& blist, int& off) {
      ::_decode(missing, blist, off);
      ::_decode(loc, blist, off);

      for (map<object_t,eversion_t>::iterator it = missing.begin();
           it != missing.end();
           it++) 
        rmissing[it->second] = it->first;
    }
  };


  /*
   * Log - incremental log of recent pg changes.
   *  also, serves as a recovery queue.
   *
   * when backlog is true, 
   *  objects with versions <= bottom are in log.
   *  we do not have any deletion info before that time, however.
   *  log is a "summary" in that it contains all objects in the PG.
   */
  class Log {
  public:
    /** top, bottom
     *    top - newest entry (update|delete)
     * bottom - entry previous to oldest (update|delete) for which we have
     *          complete negative information.  
     * i.e. we can infer pg contents for any store whose last_update >= bottom.
     */
    eversion_t top;       // newest entry (update|delete)
    eversion_t bottom;    // version prior to oldest (update|delete) 

    /** backlog - true if log is a complete summary of pg contents.  
     * updated will include all items in pg, but deleted will not include
     * negative entries for items deleted prior to 'bottom'.
     */
    bool      backlog;
    
    /** Entry
     * mapped from the eversion_t, so don't include that.
     */
    class Entry {
    public:
      const static int LOST = 0;
      const static int MODIFY = 1;
      const static int CLONE = 2;  
      const static int DELETE = 3;

      int        op;   // write, zero, trunc, remove
      object_t   oid;
      eversion_t version;
      
      osd_reqid_t reqid;  // caller+tid to uniquely identify request
      
      Entry() : op(0) {}
      Entry(int _op, object_t _oid, const eversion_t& v, 
	    const osd_reqid_t& rid) :
        op(_op), oid(_oid), version(v), reqid(rid) {}
      
      bool is_delete() const { return op == DELETE; }
      bool is_clone() const { return op == CLONE; }
      bool is_modify() const { return op == MODIFY; }
      bool is_update() const { return is_clone() || is_modify(); }
    };

    list<Entry> log;  // the actual log.

    Log() : backlog(false) {}

    void clear() {
      eversion_t z;
      top = bottom = z;
      backlog = false;
      log.clear();
    }
    bool empty() const {
      return top.version == 0 && top.epoch == 0;
    }

    void _encode(bufferlist& blist) const {
      blist.append((char*)&top, sizeof(top));
      blist.append((char*)&bottom, sizeof(bottom));
      blist.append((char*)&backlog, sizeof(backlog));
      ::_encode(log, blist);
    }
    void _decode(bufferlist& blist, int& off) {
      blist.copy(off, sizeof(top), (char*)&top);
      off += sizeof(top);
      blist.copy(off, sizeof(bottom), (char*)&bottom);
      off += sizeof(bottom);
      blist.copy(off, sizeof(backlog), (char*)&backlog);
      off += sizeof(backlog);

      ::_decode(log, blist, off);
    }

    void copy_after(const Log &other, eversion_t v);
    bool copy_after_unless_divergent(const Log &other, eversion_t split, eversion_t floor);
    void copy_non_backlog(const Log &other);
    ostream& print(ostream& out) const;
  };

  /**
   * IndexLog - adds in-memory index of the log, by oid.
   * plus some methods to manipulate it all.
   */
  class IndexedLog : public Log {
  public:
    hash_map<object_t,Entry*> objects;  // ptrs into log.  be careful!
    hash_set<osd_reqid_t>      caller_ops;

    // recovery pointers
    list<Entry>::iterator requested_to; // not inclusive of referenced item
    list<Entry>::iterator complete_to;  // not inclusive of referenced item
    
    /****/
    IndexedLog() {}

    void clear() {
      assert(0);
      unindex();
      Log::clear();
    }

    bool logged_object(object_t oid) {
      return objects.count(oid);
    }
    bool logged_req(const osd_reqid_t &r) {
      return caller_ops.count(r);
    }

    void index() {
      objects.clear();
      caller_ops.clear();
      for (list<Entry>::iterator i = log.begin();
           i != log.end();
           i++) {
        objects[i->oid] = &(*i);
        caller_ops.insert(i->reqid);
      }
    }

    void index(Entry& e) {
      if (objects.count(e.oid) == 0 || 
          objects[e.oid]->version < e.version)
        objects[e.oid] = &e;
      caller_ops.insert(e.reqid);
    }
    void unindex() {
      objects.clear();
      caller_ops.clear();
    }
    void unindex(Entry& e) {
      // NOTE: this only works if we remove from the _bottom_ of the log!
      assert(objects.count(e.oid));
      if (objects[e.oid]->version == e.version)
        objects.erase(e.oid);
      caller_ops.erase(e.reqid);
    }


    // accessors
    Entry *is_updated(object_t oid) {
      if (objects.count(oid) && objects[oid]->is_update()) return objects[oid];
      return 0;
    }
    Entry *is_deleted(object_t oid) {
      if (objects.count(oid) && objects[oid]->is_delete()) return objects[oid];
      return 0;
    }
    
    // actors
    void add(Entry& e) {
      // add to log
      log.push_back(e);
      assert(e.version > top);
      assert(top.version == 0 || e.version.version > top.version);
      top = e.version;

      // to our index
      objects[e.oid] = &(log.back());
      caller_ops.insert(e.reqid);
    }

    void trim(ObjectStore::Transaction &t, eversion_t s);
    void trim_write_ahead(eversion_t last_update);
  };
  

  /**
   * OndiskLog - some info about how we store the log on disk.
   */
  class OndiskLog {
  public:
    // ok
    off_t bottom;                     // first byte of log. 
    off_t top;                        // byte following end of log.
    map<off_t,eversion_t> block_map;  // block -> first stamp logged there

    OndiskLog() : bottom(0), top(0) {}

    bool trim_to(eversion_t v, ObjectStore::Transaction& t);
  };


  /*** PG ****/
public:
  // any
  static const int STATE_ACTIVE = 1; // i am active.  (primary: replicas too)
  
  // primary
  static const int STATE_CLEAN =  2;  // peers are complete, clean of stray replicas.
  static const int STATE_CRASHED = 4; // all replicas went down.
  static const int STATE_REPLAY = 8;  // crashed, waiting for replay
 
  // non-primary
  static const int STATE_STRAY =  16; // i must notify the primary i exist.

  static std::string get_state_string(int state) {
    std::string st;
    if (state & STATE_ACTIVE) st += "active+";
    if (state & STATE_CLEAN) st += "clean+";
    if (state & STATE_CRASHED) st += "crashed+";
    if (state & STATE_REPLAY) st += "replay+";
    if (state & STATE_STRAY) st += "stray+";
    if (!st.length()) 
      st = "inactive";
    else 
      st.resize(st.length()-1);
    return st;
  }

protected:
  OSD *osd;

  /** locking and reference counting.
   * I destroy myself when the reference count hits zero.
   * lock() should be called before doing anything.
   * get() should be called on pointer copy (to another thread, etc.).
   * put() should be called on destruction of some previously copied pointer.
   * put_unlock() when done with the current pointer (_most common_).
   */  
  Mutex _lock;
  int  ref;
  bool deleted;

public:
  void lock() {
    //cout << this << " " << info.pgid << " lock" << endl;
    _lock.Lock();
  }
  void unlock() {
    //cout << this << " " << info.pgid << " unlock" << endl;
    _lock.Unlock();
  }
  void get() {
    //cout << this << " " << info.pgid << " get " << ref << endl;
    assert(_lock.is_locked());
    ++ref; 
  }
  void put() { 
    //cout << this << " " << info.pgid << " put " << ref << endl;
    assert(_lock.is_locked());
    --ref; 
    assert(ref > 0);  // last put must be a put_unlock.
  }
  void put_unlock() { 
    //cout << this << " " << info.pgid << " put_unlock " << ref << endl;
    assert(_lock.is_locked());
    --ref; 
    _lock.Unlock();
    if (ref == 0) delete this;
  }


  list<Message*> op_queue;  // op queue


  void mark_deleted() { deleted = true; }
  bool is_deleted() { return deleted; }

public:
  // pg state
  Info        info;
  IndexedLog  log;
  OndiskLog   ondisklog;
  Missing     missing;

protected:
  int         role;    // 0 = primary, 1 = replica, -1=none.
  int         state;   // see bit defns above

  // primary state
 public:
  vector<int> acting;
  epoch_t     last_epoch_started_any;
  eversion_t  last_complete_commit;

  // [primary only] content recovery state
  eversion_t  peers_complete_thru;
  bool        have_master_log;
 protected:
  set<int>    prior_set;   // current+prior OSDs, as defined by last_epoch_started_any.
  set<int>    stray_set;   // non-acting osds that have PG data.
  set<int>    uptodate_set;  // current OSDs that are uptodate
  eversion_t  oldest_update; // lowest (valid) last_update in active set
  map<int,Info>        peer_info;   // info from peers (stray or prior)
  set<int>             peer_info_requested;
  map<int, Missing>    peer_missing;
  set<int>             peer_log_requested;  // logs i've requested (and start stamps)
  set<int>             peer_summary_requested;
  friend class OSD;


  // pg waiters
  list<class Message*>            waiting_for_active;
  hash_map<object_t, 
           list<class Message*> > waiting_for_missing_object;   
  map<eversion_t,class MOSDOp*>   replay_queue;
  
  hash_map<object_t, list<Message*> > waiting_for_wr_unlock; 

  bool block_if_wrlocked(MOSDOp* op);


  // recovery
  map<object_t, eversion_t> objects_pulling;  // which objects are currently being pulled
  

  // stats
  off_t stat_num_bytes;
  off_t stat_num_blocks;

  hash_map<object_t, DecayCounter> stat_object_temp_rd;

  Mutex pg_stats_lock;
  pg_stat_t pg_stats;

  void update_stats();

public:
  void clear_primary_state();

 public:
  bool is_acting(int osd) const { 
    for (unsigned i=0; i<acting.size(); i++)
      if (acting[i] == osd) return true;
    return false;
  }
  bool is_prior(int osd) const { return prior_set.count(osd); }
  bool is_stray(int osd) const { return stray_set.count(osd); }
  
  bool is_all_uptodate() const { return uptodate_set.size() == acting.size(); }

  void build_prior();
  void adjust_prior();  // based on new peer_info.last_epoch_started_any

  bool adjust_peers_complete_thru() {
    eversion_t t = info.last_complete;
    for (unsigned i=1; i<acting.size(); i++) 
      if (peer_info[i].last_complete < t)
        t = peer_info[i].last_complete;
    if (t > peers_complete_thru) {
      peers_complete_thru = t;
      return true;
    }
    return false;
  }

  void proc_replica_log(Log &olog, Missing& omissing, int from);
  void merge_log(Log &olog, Missing& omissing, int from);
  void proc_missing(Log &olog, Missing &omissing, int fromosd);
  
  void generate_backlog();
  void drop_backlog();
  
  void trim_write_ahead();

  void peer(ObjectStore::Transaction& t, 
	    map< int, map<pg_t,Query> >& query_map,
	    map<int, MOSDPGActivateSet*> *activator_map=0);
  void activate(ObjectStore::Transaction& t, 
		map<int, MOSDPGActivateSet*> *activator_map=0);

  virtual void clean_up_local(ObjectStore::Transaction& t) = 0;

  virtual void cancel_recovery() = 0;
  virtual bool do_recovery() = 0;
  virtual void purge_strays() = 0;

  void finish_recovery();

  off_t get_log_write_pos() {
    return 0;
  }

  friend class C_OSD_RepModify_Commit;

 public:  
  PG(OSD *o, pg_t p) : 
    osd(o), 
    ref(0), deleted(false),
    info(p),
    role(0),
    state(0),
    last_epoch_started_any(0),
    have_master_log(true),
    stat_num_bytes(0), stat_num_blocks(0)
  { }
  virtual ~PG() { }
  
  pg_t       get_pgid() const { return info.pgid; }
  int        get_nrep() const { return acting.size(); }

  int        get_primary() { return acting.empty() ? -1:acting[0]; }
  //int        get_tail() { return acting.empty() ? -1:acting[ acting.size()-1 ]; }
  //int        get_acker() { return g_conf.osd_rep == OSD_REP_PRIMARY ? get_primary():get_tail(); }
  int        get_acker() { 
    if (g_conf.osd_rep == OSD_REP_PRIMARY ||
	acting.size() <= 1) 
      return get_primary();
    return acting[1];
  }
  
  int        get_role() const { return role; }
  void       set_role(int r) { role = r; }

  bool       is_primary() const { return role == PG_ROLE_HEAD; }
  bool       is_acker() const { 
    if (g_conf.osd_rep == OSD_REP_PRIMARY)
      return is_primary();
    else
      return role == PG_ROLE_ACKER; 
  }
  bool       is_head() const { return role == PG_ROLE_HEAD; }
  bool       is_middle() const { return role == PG_ROLE_MIDDLE; }
  bool       is_residual() const { return role == PG_ROLE_STRAY; }
  
  //int  get_state() const { return state; }
  bool state_test(int m) const { return (state & m) != 0; }
  void state_set(int m) { state |= m; }
  void state_clear(int m) { state &= ~m; }

  bool is_complete() const { return info.last_complete == info.last_update; }

  bool       is_active() const { return state_test(STATE_ACTIVE); }
  bool       is_crashed() const { return state_test(STATE_CRASHED); }
  bool       is_replay() const { return state_test(STATE_REPLAY); }
  //bool       is_complete()    { return state_test(STATE_COMPLETE); }
  bool       is_clean() const { return state_test(STATE_CLEAN); }
  bool       is_stray() const { return state_test(STATE_STRAY); }

  bool  is_empty() const { return info.last_update == eversion_t(0,0); }

  int num_active_ops() const {
    return objects_pulling.size();
  }

  // pg on-disk state
  void write_log(ObjectStore::Transaction& t);
  void append_log(ObjectStore::Transaction& t, 
                  PG::Log::Entry& logentry, 
                  eversion_t trim_to);
  void read_log(ObjectStore *store);
  void trim_ondisklog_to(ObjectStore::Transaction& t, eversion_t v);


  bool is_dup(osd_reqid_t rid) {
    return log.logged_req(rid);
  }


  bool pick_missing_object_rev(object_t& oid);
  bool pick_object_rev(object_t& oid);



  // abstract bits
  virtual bool preprocess_op(MOSDOp *op, utime_t now) { return false; } 
  virtual void do_op(MOSDOp *op) = 0;
  virtual void do_sub_op(MOSDSubOp *op) = 0;
  virtual void do_sub_op_reply(MOSDSubOpReply *op) = 0;

  virtual bool same_for_read_since(epoch_t e) = 0;
  virtual bool same_for_modify_since(epoch_t e) = 0;
  virtual bool same_for_rep_modify_since(epoch_t e) = 0;

  virtual bool is_missing_object(object_t oid) = 0;
  virtual void wait_for_missing_object(object_t oid, Message *op) = 0;

  virtual void on_osd_failure(int osd) = 0;
  virtual void on_acker_change() = 0;
  virtual void on_role_change() = 0;
  virtual void on_change() = 0;
};



inline ostream& operator<<(ostream& out, const PG::Info::History& h) 
{
  return out << h.same_since << "/" << h.same_primary_since << "/" << h.same_acker_since;
}

inline ostream& operator<<(ostream& out, const PG::Info& pgi) 
{
  out << pgi.pgid << "(";
  if (pgi.is_empty())
    out << " empty";
  else
    out << " v " << pgi.last_update << "/" << pgi.last_complete
        << " (" << pgi.log_bottom << "," << pgi.last_update << "]"
        << (pgi.log_backlog ? "+backlog":"");
  out << " e " << pgi.last_epoch_started << "/" << pgi.last_epoch_finished
      << " " << pgi.history
      << ")";
  return out;
}

inline ostream& operator<<(ostream& out, const PG::Log::Entry& e)
{
  return out << " " << e.version 
             << (e.is_delete() ? " - ":
		 (e.is_clone() ? " c ":
		  (e.is_modify() ? " m ":
		   " ? ")))
             << e.oid << " by " << e.reqid;
}

inline ostream& operator<<(ostream& out, const PG::Log& log) 
{
  out << "log(" << log.bottom << "," << log.top << "]";
  if (log.backlog) out << "+backlog";
  return out;
}

inline ostream& operator<<(ostream& out, const PG::Missing& missing) 
{
  out << "missing(" << missing.num_missing();
  if (missing.num_lost()) out << ", " << missing.num_lost() << " lost";
  out << ")";
  return out;
}

inline ostream& operator<<(ostream& out, const PG& pg)
{
  out << "pg[" << pg.info 
      << " r=" << pg.get_role();

  if (pg.log.bottom != pg.info.log_bottom)
    out << " (info mismatch, " << pg.log << ")";

  if (pg.log.log.empty()) {
    // shoudl it be?
    if (pg.log.top.version - pg.log.bottom.version != 0) {
      out << " (log bound mismatch, empty)";
    }
  } else {
    if (((pg.log.log.begin()->version.version - 1 != pg.log.bottom.version) &&
         !pg.log.backlog) ||
        (pg.log.log.rbegin()->version.version != pg.log.top.version)) {
      out << " (log bound mismatch, actual=["
	  << pg.log.log.begin()->version << ","
	  << pg.log.log.rbegin()->version << "] len=" << pg.log.log.size() << ")";
    }
  }

  if (pg.get_role() == 0) out << " pct " << pg.peers_complete_thru;
  if (!pg.have_master_log) out << " !hml";
  if (pg.is_active()) out << " active";
  if (pg.is_crashed()) out << " crashed";
  if (pg.is_replay()) out << " replay";
  if (pg.is_clean()) out << " clean";
  if (pg.is_stray()) out << " stray";
  //out << " (" << pg.log.bottom << "," << pg.log.top << "]";
  if (pg.missing.num_missing()) out << " m=" << pg.missing.num_missing();
  if (pg.missing.num_lost()) out << " l=" << pg.missing.num_lost();
  out << "]";


  return out;
}



#endif
