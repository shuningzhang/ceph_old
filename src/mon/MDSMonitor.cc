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


#include "MDSMonitor.h"
#include "Monitor.h"
#include "MonitorStore.h"
#include "OSDMonitor.h"

#include "messages/MMDSMap.h"
#include "messages/MMDSGetMap.h"
#include "messages/MMDSBeacon.h"

#include "messages/MGenericMessage.h"


#include "common/Timer.h"

#include <sstream>

#include "config.h"

#define  dout(l) if (l<=g_conf.debug || l<=g_conf.debug_mon) *_dout << dbeginl << g_clock.now() << " mon" << mon->whoami << (mon->is_starting() ? (const char*)"(starting)":(mon->is_leader() ? (const char*)"(leader)":(mon->is_peon() ? (const char*)"(peon)":(const char*)"(?\?)"))) << ".mds e" << mdsmap.get_epoch() << " "
#define  derr(l) if (l<=g_conf.debug || l<=g_conf.debug_mon) *_derr << dbeginl << g_clock.now() << " mon" << mon->whoami << (mon->is_starting() ? (const char*)"(starting)":(mon->is_leader() ? (const char*)"(leader)":(mon->is_peon() ? (const char*)"(peon)":(const char*)"(?\?)"))) << ".mds e" << mdsmap.get_epoch() << " "



// my methods

void MDSMonitor::print_map(MDSMap &m, int dbl)
{
  dout(7) << "print_map epoch " << m.get_epoch() << " max " << m.max_mds << dendl;
  entity_inst_t blank;
  set<int> all;
  m.get_mds_set(all);
  for (set<int>::iterator p = all.begin();
       p != all.end();
       ++p) {
    if (m.standby_for.count(*p) && !m.standby_for[*p].empty()) {
      dout(7) << " mds" << *p << "." << m.mds_inc[*p]
	      << " : " << MDSMap::get_state_name(m.get_state(*p))
	      << " : " << (m.have_inst(*p) ? m.get_inst(*p) : blank)
	      << " : +" << m.standby_for[*p].size()
	      << " standby " << m.standby_for[*p]
	      << dendl;
    } else {
      dout(7) << " mds" << *p << "." << m.mds_inc[*p]
	      << " : " << MDSMap::get_state_name(m.get_state(*p))
	      << " : " << (m.have_inst(*p) ? m.get_inst(*p) : blank)
	      << dendl;
    }
  }
  if (!m.standby_any.empty()) {
    dout(7) << " +" << m.standby_any.size() << " shared standby " << m.standby_any << dendl;
  }
}



// service methods

void MDSMonitor::create_initial()
{
  dout(10) << "create_initial" << dendl;
  pending_mdsmap.max_mds = g_conf.num_mds;
  pending_mdsmap.created = g_clock.now();
  print_map(pending_mdsmap);
}

bool MDSMonitor::update_from_paxos()
{
  assert(paxos->is_active());

  version_t paxosv = paxos->get_version();
  if (paxosv == mdsmap.epoch) return true;
  assert(paxosv >= mdsmap.epoch);

  dout(10) << "update_from_paxos paxosv " << paxosv 
	   << ", my e " << mdsmap.epoch << dendl;

  // read and decode
  mdsmap_bl.clear();
  bool success = paxos->read(paxosv, mdsmap_bl);
  assert(success);
  dout(10) << "update_from_paxos  got " << paxosv << dendl;
  mdsmap.decode(mdsmap_bl);

  // new map
  dout(4) << "new map" << dendl;
  print_map(mdsmap, 0);

  // bcast map to mds, waiters
  if (mon->is_leader())
    bcast_latest_mds();
  send_to_waiting();

  // make sure last_beacon is populated
  for (map<int32_t,entity_inst_t>::iterator p = mdsmap.mds_inst.begin();
       p != mdsmap.mds_inst.end();
       ++p) 
    if (last_beacon.count(p->second.addr) == 0 &&
	mdsmap.get_state(p->first) != MDSMap::STATE_DNE &&
	mdsmap.get_state(p->first) != MDSMap::STATE_STOPPED &&
	mdsmap.get_state(p->first) != MDSMap::STATE_FAILED)
      last_beacon[p->second.addr] = g_clock.now();
  for (map<entity_addr_t,int32_t>::iterator p = mdsmap.standby.begin();
       p != mdsmap.standby.end();
       ++p )
    if (last_beacon.count(p->first) == 0)
      last_beacon[p->first] = g_clock.now();

  return true;
}

void MDSMonitor::create_pending()
{
  pending_mdsmap = mdsmap;
  pending_mdsmap.epoch++;
  dout(10) << "create_pending e" << pending_mdsmap.epoch << dendl;
}

void MDSMonitor::encode_pending(bufferlist &bl)
{
  dout(10) << "encode_pending e" << pending_mdsmap.epoch << dendl;
  
  //print_map(pending_mdsmap);

  // apply to paxos
  assert(paxos->get_version() + 1 == pending_mdsmap.epoch);
  pending_mdsmap.encode(bl);
}


bool MDSMonitor::preprocess_query(Message *m)
{
  dout(10) << "preprocess_query " << *m << " from " << m->get_source_inst() << dendl;

  switch (m->get_type()) {
    
  case MSG_MDS_BEACON:
    return preprocess_beacon((MMDSBeacon*)m);
    
  case CEPH_MSG_MDS_GETMAP:
    handle_mds_getmap((MMDSGetMap*)m);
    return true;

  default:
    assert(0);
    delete m;
    return true;
  }
}

void MDSMonitor::handle_mds_getmap(MMDSGetMap *m)
{
  if (m->have < mdsmap.get_epoch())
    send_full(m->get_source_inst());
  else
    waiting_for_map.push_back(m->get_source_inst());
}


bool MDSMonitor::preprocess_beacon(MMDSBeacon *m)
{
  dout(12) << "preprocess_beacon " << *m
	   << " from " << m->get_mds_inst()
	   << dendl;

  // fw to leader?
  if (!mon->is_leader()) {
    dout(10) << "fw to leader" << dendl;
    mon->messenger->send_message(m, mon->monmap->get_inst(mon->get_leader()));
    return true;
  }

  // let's see.
  int from = m->get_mds_inst().name.num();
  entity_addr_t addr = m->get_mds_inst().addr;
  int state = m->get_state();
  version_t seq = m->get_seq();

  // can i handle this query without a map update?
  
  // boot?
  if (state == MDSMap::STATE_BOOT) {
    // already booted?
    if (pending_mdsmap.get_addr_rank(addr) == -1)
      return false; // not booted|booting|standby yet

    // ignore.
    goto out;
  }
  else if (state == MDSMap::STATE_STANDBY) {
    // standby?
    if (!pending_mdsmap.is_standby(addr) &&
	!mdsmap.is_standby(addr)) {
      dout(7) << "mds_beacon " << *m << " claiming standby, but not, ignoring" << dendl;
      goto out;
    }
    // reply.
  }
  else {
    // old seq?
    if (mdsmap.mds_state_seq[from] > seq) {
      dout(7) << "mds_beacon " << *m << " has old seq, ignoring" << dendl;
      goto out;
    }
    
    // is there a state change here?
    if (mdsmap.mds_state.count(from) == 0) { 
      dout(1) << "mds_beacon " << *m << " announcing non-boot|standby state, ignoring" << dendl;
      goto out;
    }

    if (mdsmap.mds_state[from] != state) {
      if (mdsmap.get_epoch() == m->get_last_epoch_seen()) 
	return false;  // need to update map
      dout(10) << "mds_beacon " << *m << " ignoring requested state, because mds hasn't seen latest map" << dendl;
    }
  }

  // note time and reply
  dout(15) << "mds_beacon " << *m << " noting time and replying" << dendl;
  last_beacon[addr] = g_clock.now();  
  mon->messenger->send_message(new MMDSBeacon(m->get_mds_inst(), mdsmap.get_epoch(), state, seq, 0), 
			       m->get_mds_inst());

  // done
 out:
  delete m;
  return true;
}


bool MDSMonitor::prepare_update(Message *m)
{
  dout(7) << "prepare_update " << *m << dendl;

  switch (m->get_type()) {
    
  case MSG_MDS_BEACON:
    return handle_beacon((MMDSBeacon*)m);
    
  default:
    assert(0);
    delete m;
  }

  return true;
}



bool MDSMonitor::handle_beacon(MMDSBeacon *m)
{
  // -- this is an update --
  dout(12) << "handle_beacon " << *m
	   << " from " << m->get_mds_inst()
	   << dendl;
  int from = m->get_mds_inst().name.num();
  entity_addr_t addr = m->get_mds_inst().addr;
  int state = m->get_state();
  version_t seq = m->get_seq();

  // boot?
  int standby_for = -1;
  if (state == MDSMap::STATE_BOOT) {
    from = -1;  

    // standby for a given rank?
    standby_for = m->get_want_rank();
    if (standby_for >= pending_mdsmap.max_mds) {
      dout(10) << "mds_beacon boot: wanted standby for mds" << from 
	       << " >= max_mds " << pending_mdsmap.max_mds
	       << ", will be shared standby" << dendl;
      standby_for = -1;
    }
    if (standby_for >= 0 && pending_mdsmap.is_down(standby_for)) {
      // wants to be a specific MDS, who is down
      from = standby_for;
      switch (pending_mdsmap.get_state(standby_for)) {
      case MDSMap::STATE_STOPPED:
	state = MDSMap::STATE_STARTING;
	break;
      case MDSMap::STATE_DNE:
	state = MDSMap::STATE_CREATING;
	break;
      case MDSMap::STATE_FAILED:
	state = MDSMap::STATE_REPLAY;
	break;
      default:
	assert(0);
      }
      dout(10) << "mds_beacon boot: mds" << from
	       << " was " << MDSMap::get_state_name(pending_mdsmap.get_state(from))
	       << ", " << MDSMap::get_state_name(state) 
	       << dendl;
    }
    else if (standby_for < 0) {
      // pick another failed mds?
      set<int> failed;
      pending_mdsmap.get_failed_mds_set(failed);
      if (!failed.empty()) {
	from = *failed.begin();
	dout(10) << "mds_beacon boot: assigned failed mds" << from << dendl;
	state = MDSMap::STATE_REPLAY;
      }
    }
    if (from < 0 && standby_for < 0 && 
	!pending_mdsmap.is_degraded()) {
      // ok, just pick any unused mds rank
      //  that doesn't make us overfull
      for (int i=0; i<pending_mdsmap.max_mds; i++) {
	if (pending_mdsmap.would_be_overfull_with(i)) continue;
	if (pending_mdsmap.is_dne(i)) {
	  from = i;
	  dout(10) << "mds_beacon boot: assigned new mds" << from << dendl;
	  state = MDSMap::STATE_CREATING;
	  break;
	} else if (pending_mdsmap.is_stopped(i)) {
	  from = i;
	  dout(10) << "mds_beacon boot: assigned stopped mds" << from << dendl;
	  state = MDSMap::STATE_STARTING;
	  break;
	}
      }
    }

    if (from < 0) {
      // standby
      if (standby_for < 0) {
	dout(10) << "mds_beacon boot: standby for any" << dendl;
	pending_mdsmap.standby_any.insert(addr);
      } else {
	dout(10) << "mds_beacon boot: standby for mds" << standby_for << dendl;
	pending_mdsmap.standby_for[standby_for].insert(addr);
      }
      pending_mdsmap.standby[addr] = standby_for;
      state = MDSMap::STATE_STANDBY;
    } else {
      // join|takeover
      assert(state == MDSMap::STATE_CREATING ||
	     state == MDSMap::STATE_STARTING ||
	     state == MDSMap::STATE_REPLAY);
    
      pending_mdsmap.mds_inst[from].addr = addr;
      pending_mdsmap.mds_inst[from].name = entity_name_t::MDS(from);
      pending_mdsmap.mds_inc[from]++;
      pending_mdsmap.mds_state[from] = state;
      pending_mdsmap.mds_state_seq[from] = seq;
    }

    // initialize the beacon timer
    last_beacon[addr] = g_clock.now();

  } else {
    // state change
    dout(10) << "mds_beacon mds" << from << " " << MDSMap::get_state_name(mdsmap.mds_state[from])
	     << " -> " << MDSMap::get_state_name(state)
	     << dendl;

    // change the state
    pending_mdsmap.mds_state[from] = state;
    if (pending_mdsmap.is_up(from))
      pending_mdsmap.mds_state_seq[from] = seq;
    else
      pending_mdsmap.mds_state_seq.erase(from);
  }

  dout(7) << "pending map now:" << dendl;
  print_map(pending_mdsmap);
  
  paxos->wait_for_commit(new C_Updated(this, from, m));

  return true;
}

bool MDSMonitor::should_propose(double& delay)
{
  delay = 0.0;
  return true;
}

void MDSMonitor::_updated(int from, MMDSBeacon *m)
{
  if (from < 0) {
    dout(10) << "_updated (booted) mds" << from << " " << *m << dendl;
    mon->osdmon->send_latest(m->get_source_inst());
  } else {
    dout(10) << "_updated mds" << from << " " << *m << dendl;
  }
  if (m->get_state() == MDSMap::STATE_STOPPED) {
    // send the map manually (they're out of the map, so they won't get it automatic)
    send_latest(m->get_mds_inst());
  }

  delete m;
}


void MDSMonitor::committed()
{
  // check for failed
  set<int> failed;
  mdsmap.get_failed_mds_set(failed);

  if (!mdsmap.standby.empty() && !failed.empty()) {
    bool didtakeover = false;
    set<int>::iterator p = failed.begin();
    while (p != failed.end()) {
      int f = *p++;
      
      // someone standby for me?
      if (mdsmap.standby_for.count(f) &&
	  !mdsmap.standby_for[f].empty()) {
	dout(0) << "mds" << f << " standby " << *mdsmap.standby_for[f].begin() << " taking over" << dendl;
	take_over(*mdsmap.standby_for[f].begin(), f);
	didtakeover = true;
      }
      else if (!mdsmap.standby_any.empty()) {
	dout(0) << "standby " << mdsmap.standby.begin()->first << " taking over for mds" << f << dendl;
	take_over(mdsmap.standby.begin()->first, f);
	didtakeover = true;
      }
    }
    if (didtakeover) {
      dout(7) << "pending map now:" << dendl;
      print_map(pending_mdsmap);
      propose_pending();
    }
  }

  // hackish: did all mds's shut down?
  if (mon->is_leader() &&
      g_conf.mon_stop_with_last_mds &&
      mdsmap.get_epoch() > 1 &&
      mdsmap.is_stopped()) 
    mon->messenger->send_message(new MGenericMessage(CEPH_MSG_SHUTDOWN), 
				 mon->monmap->get_inst(mon->whoami));
}

void MDSMonitor::take_over(entity_addr_t addr, int mds)
{
  pending_mdsmap.mds_inst[mds].addr = addr;
  pending_mdsmap.mds_inst[mds].name = entity_name_t::MDS(mds);
  pending_mdsmap.mds_inc[mds]++;
  pending_mdsmap.mds_state[mds] = MDSMap::STATE_REPLAY;
  pending_mdsmap.mds_state_seq[mds] = 0;

  // remove from standby list(s)
  pending_mdsmap.standby.erase(addr);
  pending_mdsmap.standby_for[mds].erase(addr);
  pending_mdsmap.standby_any.erase(addr);
}



int MDSMonitor::do_command(vector<string>& cmd, bufferlist& data, 
			   bufferlist& rdata, string &rs)
{
  int r = -EINVAL;
  stringstream ss;

  if (cmd.size() > 1) {
    if (cmd[1] == "stop" && cmd.size() > 2) {
      int who = atoi(cmd[2].c_str());
      if (mdsmap.is_active(who)) {
	r = 0;
	ss << "telling mds" << who << " to stop";
	pending_mdsmap.mds_state[who] = MDSMap::STATE_STOPPING;
      } else {
	r = -EEXIST;
	ss << "mds" << who << " not active (" << mdsmap.get_state_name(mdsmap.get_state(who)) << ")";
      }
    }
    else if (cmd[1] == "set_max_mds" && cmd.size() > 2) {
      pending_mdsmap.max_mds = atoi(cmd[2].c_str());
      r = 0;
      ss << "max_mds = " << pending_mdsmap.max_mds;
    }
  }
  if (r == -EINVAL) {
    ss << "unrecognized command";
  } 
  
  // reply
  getline(ss, rs);
  return r;
}



void MDSMonitor::bcast_latest_mds()
{
  dout(10) << "bcast_latest_mds " << mdsmap.get_epoch() << dendl;
  
  // tell mds
  set<int> up;
  mdsmap.get_up_mds_set(up);
  for (set<int>::iterator p = up.begin();
       p != up.end();
       p++) 
    send_full(mdsmap.get_inst(*p));

  // standby too
  entity_inst_t inst;
  inst.name = entity_name_t::MDS(-1);
  for (map<entity_addr_t,int32_t>::iterator p = mdsmap.standby.begin();
       p != mdsmap.standby.end();
       p++) {
    inst.addr = p->first;
    send_full(inst);
  }
}

void MDSMonitor::send_full(entity_inst_t dest)
{
  dout(11) << "send_full to " << dest << dendl;
  mon->messenger->send_message(new MMDSMap(&mdsmap), dest);
}

void MDSMonitor::send_to_waiting()
{
  dout(10) << "send_to_waiting " << mdsmap.get_epoch() << dendl;
  for (list<entity_inst_t>::iterator i = waiting_for_map.begin();
       i != waiting_for_map.end();
       i++) 
    send_full(*i);
  waiting_for_map.clear();
}

void MDSMonitor::send_latest(entity_inst_t dest)
{
  if (paxos->is_readable()) 
    send_full(dest);
  else
    waiting_for_map.push_back(dest);
}


void MDSMonitor::tick()
{
  // make sure mds's are still alive
  // ...if i am an active leader
  if (!mon->is_leader()) return;
  if (!paxos->is_active()) return;

  utime_t cutoff = g_clock.now();
  cutoff -= g_conf.mds_beacon_grace;
    
  map<entity_addr_t, utime_t>::iterator p = last_beacon.begin();
  while (p != last_beacon.end()) {
    entity_addr_t addr = p->first;
    p++;

    if (last_beacon[addr] >= cutoff) continue;

    int mds = pending_mdsmap.get_addr_rank(addr);
    if (mds >= 0) {
      // failure!
      int curstate = pending_mdsmap.get_state(mds);
      int newstate = curstate;
      switch (curstate) {
      case MDSMap::STATE_CREATING:
	newstate = MDSMap::STATE_DNE;	// didn't finish creating
	last_beacon.erase(addr);
	break;

      case MDSMap::STATE_STARTING:
	newstate = MDSMap::STATE_STOPPED;
	break;

      case MDSMap::STATE_STOPPED:
	break;

      case MDSMap::STATE_REPLAY:
      case MDSMap::STATE_RESOLVE:
      case MDSMap::STATE_RECONNECT:
      case MDSMap::STATE_REJOIN:
      case MDSMap::STATE_ACTIVE:
      case MDSMap::STATE_STOPPING:
	newstate = MDSMap::STATE_FAILED;
	break;

      default:
	assert(0);
      }
	  
      dout(10) << "no beacon from mds" << *p << " since " << last_beacon[addr]
	       << ", marking " << pending_mdsmap.get_state_name(newstate)
	       << dendl;
      
      // update map
      pending_mdsmap.mds_state[mds] = newstate;
      pending_mdsmap.mds_state_seq.erase(mds);
    } 
    else if (pending_mdsmap.is_standby(addr)) {
      dout(10) << "no beacon from standby " << addr << " since " << last_beacon[addr]
	       << ", removing from standby list"
	       << dendl;
      if (pending_mdsmap.standby[addr] >= 0)
	pending_mdsmap.standby_for[pending_mdsmap.standby[addr]].erase(addr);
      else
	pending_mdsmap.standby_any.erase(addr);
      pending_mdsmap.standby.erase(addr);
    } 
    else {
      dout(0) << "BUG: removing stray " << addr << " from last_beacon map" << dendl;
    }

    last_beacon.erase(addr);
    propose_pending();
  }
}


void MDSMonitor::do_stop()
{
  // hrm...
  if (!mon->is_leader() ||
      !paxos->is_active()) {
    dout(-10) << "do_stop can't stop right now, mdsmap not writeable" << dendl;
    return;
  }

  dout(7) << "do_stop stopping active mds nodes" << dendl;
  print_map(mdsmap);

  for (map<int32_t,int32_t>::iterator p = mdsmap.mds_state.begin();
       p != mdsmap.mds_state.end();
       ++p) {
    switch (p->second) {
    case MDSMap::STATE_ACTIVE:
    case MDSMap::STATE_STOPPING:
      pending_mdsmap.mds_state[p->first] = MDSMap::STATE_STOPPING;
      break;
    case MDSMap::STATE_CREATING:
      pending_mdsmap.mds_state[p->first] = MDSMap::STATE_DNE;
      last_beacon.erase(pending_mdsmap.mds_inst[p->first].addr);
      break;
    case MDSMap::STATE_STARTING:
      pending_mdsmap.mds_state[p->first] = MDSMap::STATE_STOPPED;
      break;
    case MDSMap::STATE_REPLAY:
    case MDSMap::STATE_RESOLVE:
    case MDSMap::STATE_RECONNECT:
    case MDSMap::STATE_REJOIN:
      // BUG: hrm, if this is the case, the STOPPING gusy won't be able to stop, will they?
      pending_mdsmap.mds_state[p->first] = MDSMap::STATE_FAILED;
      break;
    }
  }
  // hose standby list
  pending_mdsmap.standby.clear();
  pending_mdsmap.standby_for.clear();
  pending_mdsmap.standby_any.clear();

  propose_pending();
}
