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

#ifndef __MDS_SESSIONMAP_H
#define __MDS_SESSIONMAP_H

#include <set>
using std::set;

#include <ext/hash_map>
using __gnu_cxx::hash_map;

#include "include/Context.h"
#include "include/xlist.h"
#include "mdstypes.h"

class CInode;
class Capability;

/* 
 * session
 */

class Session {
  // -- state etc --
public:
  static const int STATE_UNDEF = 0;
  static const int STATE_OPENING = 1;   // journaling open
  static const int STATE_OPEN = 2;
  static const int STATE_CLOSING = 3;   // journaling close
  static const int STATE_STALE = 4;
  static const int STATE_STALE_CLOSING = 5;
  //static const int STATE_RECONNECTING = 5; // ?

private:
  int state;
  friend class SessionMap;
public:
  entity_inst_t inst;
  xlist<Session*>::item session_list_item;

  bool is_undef() { return state == STATE_UNDEF; }
  bool is_opening() { return state == STATE_OPENING; }
  bool is_open() { return state == STATE_OPEN; }
  bool is_closing() { return state == STATE_CLOSING; }
  bool is_stale() { return state == STATE_STALE; }
  bool is_stale_closing() { return state == STATE_STALE_CLOSING; }

  // -- caps --
private:
  version_t cap_push_seq;        // cap push seq #
public:
  xlist<Capability*> caps;  // inodes with caps; front=most recently used
  utime_t last_cap_renew;

public:
  version_t inc_push_seq() { return ++cap_push_seq; }
  version_t get_push_seq() const { return cap_push_seq; }

  // -- completed requests --
private:
  set<tid_t> completed_requests;
  map<tid_t, Context*> waiting_for_trim;

public:
  void add_completed_request(tid_t t) {
    completed_requests.insert(t);
  }
  void trim_completed_requests(tid_t mintid) {
    // trim
    while (!completed_requests.empty() && 
	   (mintid == 0 || *completed_requests.begin() < mintid))
      completed_requests.erase(completed_requests.begin());

    // kick waiters
    list<Context*> fls;
    while (!waiting_for_trim.empty() &&
	   (mintid == 0 || waiting_for_trim.begin()->first < mintid)) {
      fls.push_back(waiting_for_trim.begin()->second);
      waiting_for_trim.erase(waiting_for_trim.begin());
    }
    finish_contexts(fls);
  }
  void add_trim_waiter(tid_t tid, Context *c) {
    waiting_for_trim[tid] = c;
  }
  bool have_completed_request(tid_t tid) const {
    return completed_requests.count(tid);
  }


  Session() : 
    state(STATE_UNDEF), 
    session_list_item(this),
    cap_push_seq(0) { }

  void _encode(bufferlist& bl) const {
    ::_encode_simple(inst, bl);
    ::_encode_simple(cap_push_seq, bl);
    ::_encode_simple(completed_requests, bl);
  }
  void _decode(bufferlist::iterator& p) {
    ::_decode_simple(inst, p);
    ::_decode_simple(cap_push_seq, p);
    ::_decode_simple(completed_requests, p);
  }
};


/*
 * session map
 */

class MDS;

class SessionMap {
private:
  MDS *mds;
  hash_map<entity_name_t, Session*> session_map;
public:
  map<int,xlist<Session*> > by_state;
  
public:  // i am lazy
  version_t version, projected, committing, committed;
  map<version_t, list<Context*> > commit_waiters;

public:
  SessionMap(MDS *m) : mds(m), 
		       version(0), projected(0), committing(0), committed(0) 
  { }
    
  // sessions
  bool empty() { return session_map.empty(); }
  Session* get_session(entity_name_t w) {
    if (session_map.count(w))
      return session_map[w];
    return 0;
  }
  Session* get_or_add_session(entity_inst_t i) {
    if (session_map.count(i.name))
      return session_map[i.name];
    Session *s = session_map[i.name] = new Session;
    s->inst = i;
    return s;
  }
  void remove_session(Session *s) {
    s->trim_completed_requests(0);
    session_map.erase(s->inst.name);
    delete s;
  }
  void touch_session(Session *s) {
    s->last_cap_renew = g_clock.now();
  }
  Session *get_oldest_session(int state) {
    if (by_state[state].empty()) return 0;
    return by_state[state].front();
  }
  void set_state(Session *session, int s) {
    if (session->state != s) {
      session->state = s;
      by_state[s].push_back(&session->session_list_item);
    }
  }
  void dump();

  void get_client_set(set<int>& s) {
    for (hash_map<entity_name_t,Session*>::iterator p = session_map.begin();
	 p != session_map.end();
	 p++)
      if (p->second->inst.name.is_client())
	s.insert(p->second->inst.name.num());
  }
  void get_client_session_set(set<Session*>& s) {
    for (hash_map<entity_name_t,Session*>::iterator p = session_map.begin();
	 p != session_map.end();
	 p++)
      if (p->second->inst.name.is_client())
	s.insert(p->second);
  }

  void open_sessions(map<int,entity_inst_t>& client_map) {
    for (map<int,entity_inst_t>::iterator p = client_map.begin(); 
	 p != client_map.end(); 
	 ++p) {
      Session *session = get_or_add_session(p->second);
      session->inst = p->second;
      set_state(session, Session::STATE_OPEN);
    }
    version++;
  }

  // helpers
  entity_inst_t& get_inst(entity_name_t w) {
    assert(session_map.count(w));
    return session_map[w]->inst;
  }
  version_t inc_push_seq(int client) {
    return get_session(entity_name_t::CLIENT(client))->inc_push_seq();
  }
  version_t get_push_seq(int client) {
    return get_session(entity_name_t::CLIENT(client))->get_push_seq();
  }
  bool have_completed_request(metareqid_t rid) {
    Session *session = get_session(rid.name);
    return session && session->have_completed_request(rid.tid);
  }
  void add_completed_request(metareqid_t rid) {
    Session *session = get_session(rid.name);
    assert(session);
    session->add_completed_request(rid.tid);
  }
  void trim_completed_requests(entity_name_t c, tid_t tid) {
    Session *session = get_session(c);
    assert(session);
    session->trim_completed_requests(tid);
  }

  // -- loading, saving --
  inode_t inode;
  list<Context*> waiting_for_load;

  void encode(bufferlist& bl);
  void decode(bufferlist::iterator& blp);

  void init_inode();
  void load(Context *onload);
  void _load_finish(bufferlist &bl);
  void save(Context *onsave, version_t needv=0);
  void _save_finish(version_t v);
 
};


#endif
