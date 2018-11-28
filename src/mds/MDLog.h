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


#ifndef __MDLOG_H
#define __MDLOG_H

#include "include/types.h"
#include "include/Context.h"

#include "common/Thread.h"
#include "common/Cond.h"

#include "LogSegment.h"

#include <list>

//#include <ext/hash_map>
//using __gnu_cxx::hash_mapset;

class Journaler;
class LogEvent;
class MDS;
class LogSegment;
class ESubtreeMap;

class Logger;

#include <map>
using std::map;


class MDLog {
 protected:
  MDS *mds;
  int num_events; // in events
  int max_events;
  int max_segments;

  int unflushed;

  bool capped;

  inode_t log_inode;
  Journaler *journaler;

  Logger *logger;


  // -- replay --
  Cond replay_cond;

  class ReplayThread : public Thread {
    MDLog *log;
  public:
    ReplayThread(MDLog *l) : log(l) {}
    void* entry() {
      log->_replay_thread();
      return 0;
    }
  } replay_thread;

  friend class ReplayThread;
  friend class C_MDL_Replay;

  list<Context*> waitfor_replay;

  void _replay();         // old way
  void _replay_thread();  // new way


  // -- segments --
  map<off_t,LogSegment*> segments;
  set<LogSegment*> expiring_segments;
  set<LogSegment*> expired_segments;
  int expiring_events;
  int expired_events;

  class C_MDL_WroteSubtreeMap : public Context {
    MDLog *mdlog;
    off_t off;
  public:
    C_MDL_WroteSubtreeMap(MDLog *l, off_t o) : mdlog(l), off(o) { }
    void finish(int r) {
      mdlog->_logged_subtree_map(off);
    }
  };
  void _logged_subtree_map(off_t off);


  // -- subtreemaps --
  bool writing_subtree_map;  // one is being written now

  friend class ESubtreeMap;
  friend class C_MDS_WroteImportMap;
  friend class MDCache;

public:
  off_t get_last_segment_offset() {
    assert(!segments.empty());
    return segments.rbegin()->first;
  }


private:
  void init_journaler();
  
public:
  void reopen_logger(utime_t start, bool append=false);
  
  // replay state
  map<inodeno_t, set<inodeno_t> >   pending_exports;



public:
  MDLog(MDS *m) : mds(m),
		  num_events(0), 
		  max_events(g_conf.mds_log_max_events),
		  max_segments(g_conf.mds_log_max_segments),
		  unflushed(0),
		  capped(false),
		  journaler(0),
		  logger(0),
		  replay_thread(this),
		  expiring_events(0), expired_events(0),
		  writing_subtree_map(false) {
  }		  
  ~MDLog();


  void start_new_segment(Context *onsync=0);
  LogSegment *get_current_segment() { 
    return segments.empty() ? 0:segments.rbegin()->second; 
  }


  void flush_logger();

  size_t get_num_events() { return num_events; }
  void set_max_events(int m) { max_events = m; }
  size_t get_num_segments() { return segments.size(); }  
  void set_max_segments(int m) { max_segments = m; }

  off_t get_read_pos();
  off_t get_write_pos();
  bool empty() { return segments.empty(); }

  bool is_capped() { return capped; }
  void cap();

  void submit_entry( LogEvent *e, Context *c = 0 );
  void wait_for_sync( Context *c );
  void flush();
  bool is_flushed() {
    return unflushed == 0;
  }

private:
  class C_MaybeExpiredSegment : public Context {
    MDLog *mdlog;
    LogSegment *ls;
  public:
    C_MaybeExpiredSegment(MDLog *mdl, LogSegment *s) : mdlog(mdl), ls(s) {}
    void finish(int res) {
      mdlog->_maybe_expired(ls);
    }
  };

  void try_expire(LogSegment *ls);
  void _maybe_expired(LogSegment *ls);
  void _expired(LogSegment *ls);

public:
  void trim();

private:
  void write_head(Context *onfinish);

public:
  void create(Context *onfinish);  // fresh, empty log! 
  void open(Context *onopen);      // append() or replay() to follow!
  void append();
  void replay(Context *onfinish);
};

#endif
