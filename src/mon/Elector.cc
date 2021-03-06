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

#include "Elector.h"
#include "Monitor.h"

#include "common/Timer.h"
#include "MonitorStore.h"
#include "messages/MMonElection.h"

#include "config.h"

#define  dout(l) if (l<=g_conf.debug || l<=g_conf.debug_mon) *_dout << dbeginl << g_clock.now() << " mon" << mon->whoami << (mon->is_starting() ? (const char*)"(starting)":(mon->is_leader() ? (const char*)"(leader)":(mon->is_peon() ? (const char*)"(peon)":(const char*)"(?\?)"))) << ".elector(" << epoch << ") "
#define  derr(l) if (l<=g_conf.debug || l<=g_conf.debug_mon) *_derr << dbeginl << g_clock.now() << " mon" << mon->whoami << (mon->is_starting() ? (const char*)"(starting)":(mon->is_leader() ? (const char*)"(leader)":(mon->is_peon() ? (const char*)"(peon)":(const char*)"(?\?)"))) << ".elector(" << epoch << ") "


void Elector::init()
{
  epoch = mon->store->get_int("mon_epoch");
  if (!epoch)
    epoch = 1;
  dout(1) << "init, last seen epoch " << epoch << dendl;
}

void Elector::shutdown()
{
  if (expire_event)
    mon->timer.cancel_event(expire_event);
}

void Elector::bump_epoch(epoch_t e) 
{
  dout(10) << "bump_epoch " << epoch << " to " << e << dendl;
  assert(epoch < e);
  epoch = e;
  mon->store->put_int(epoch, "mon_epoch");

  // clear up some state
  electing_me = false;
  acked_me.clear();
  leader_acked = -1;
}


void Elector::start()
{
  dout(5) << "start -- can i be leader?" << dendl;
  
  // start by trying to elect me
  if (epoch % 2 == 0) 
    bump_epoch(epoch+1);  // odd == election cycle
  start_stamp = g_clock.now();
  electing_me = true;
  acked_me.insert(whoami);
  
  // bcast to everyone else
  for (unsigned i=0; i<mon->monmap->size(); ++i) {
    if ((int)i == whoami) continue;
    mon->messenger->send_message(new MMonElection(MMonElection::OP_PROPOSE, epoch),
				 mon->monmap->get_inst(i));
  }
  
  reset_timer();
}

void Elector::defer(int who)
{
  dout(5) << "defer to " << who << dendl;

  if (electing_me) {
    // drop out
    acked_me.clear();
    electing_me = false;
  }

  // ack them
  leader_acked = who;
  ack_stamp = g_clock.now();
  mon->messenger->send_message(new MMonElection(MMonElection::OP_ACK, epoch),
			       mon->monmap->get_inst(who));
  
  // set a timer
  reset_timer(1.0);  // give the leader some extra time to declare victory
}


void Elector::reset_timer(double plus)
{
  // set the timer
  cancel_timer();
  expire_event = new C_ElectionExpire(this);
  mon->timer.add_event_after(g_conf.mon_lease + plus,
			     expire_event);
}


void Elector::cancel_timer()
{
  if (expire_event) {
    mon->timer.cancel_event(expire_event);
    expire_event = 0;
  }
}

void Elector::expire()
{
  dout(5) << "election timer expired" << dendl;
  
  // did i win?
  if (electing_me &&
      acked_me.size() > (unsigned)(mon->monmap->size() / 2)) {
    // i win
    victory();
  } else {
    // whoever i deferred to didn't declare victory quickly enough.
    start();
  }
}


void Elector::victory()
{
  leader_acked = -1;
  electing_me = false;
  set<int> quorum = acked_me;
  
  cancel_timer();
  
  assert(epoch % 2 == 1);  // election
  bump_epoch(epoch+1);     // is over!
  
  // tell everyone
  for (set<int>::iterator p = quorum.begin();
       p != quorum.end();
       ++p) {
    if (*p == whoami) continue;
    mon->messenger->send_message(new MMonElection(MMonElection::OP_VICTORY, epoch),
				 mon->monmap->get_inst(*p));
  }
    
  // tell monitor
  mon->win_election(epoch, quorum);
}


void Elector::handle_propose(MMonElection *m)
{
  dout(5) << "handle_propose from " << m->get_source() << dendl;
  int from = m->get_source().num();

  assert(m->epoch % 2 == 1); // election
  if (m->epoch > epoch) {
    bump_epoch(m->epoch);
  }
  else if (m->epoch < epoch &&  // got an "old" propose,
	   epoch % 2 == 0 &&    // in a non-election cycle
	   mon->quorum.count(from) == 0) {  // from someone outside the quorum
    // a mon just started up, call a new election so they can rejoin!
    dout(5) << " got propose from old epoch, " << m->get_source() << " must have just started" << dendl;
    start();
  }

  if (whoami < from) {
    // i would win over them.
    if (leader_acked >= 0) {        // we already acked someone
      assert(leader_acked < from);  // and they still win, of course
      dout(5) << "no, we already acked " << leader_acked << dendl;
    } else {
      // wait, i should win!
      if (!electing_me)
	start();
    }
  } else {
    // they would win over me
    if (leader_acked < 0 ||      // haven't acked anyone yet, or
	leader_acked > from ||   // they would win over who you did ack, or
	leader_acked == from) {  // this is the guy we're already deferring to
      defer(from);
    } else {
      // ignore them!
      dout(5) << "no, we already acked " << leader_acked << dendl;
    }
  }
  
  delete m;
}
 
void Elector::handle_ack(MMonElection *m)
{
  dout(5) << "handle_ack from " << m->get_source() << dendl;
  int from = m->get_source().num();
  
  assert(m->epoch % 2 == 1); // election
  if (m->epoch > epoch) {
    dout(5) << "woah, that's a newer epoch, i must have rebooted.  bumping and re-starting!" << dendl;
    bump_epoch(m->epoch);
    start();
    delete m;
    return;
  }
  assert(m->epoch == epoch);
  
  if (electing_me) {
    // thanks
    acked_me.insert(from);
    dout(5) << " so far i have " << acked_me << dendl;
    
    // is that _everyone_?
    if (acked_me.size() == mon->monmap->size()) {
      // if yes, shortcut to election finish
      victory();
    }
  } else {
    // ignore, i'm deferring already.
    assert(leader_acked >= 0);
  }
  
  delete m;
}


void Elector::handle_victory(MMonElection *m)
{
  dout(5) << "handle_victory from " << m->get_source() << dendl;
  int from = m->get_source().num();

  assert(from < whoami);
  assert(m->epoch % 2 == 0);  
  assert(m->epoch == epoch + 1);  // i should have seen this election if i'm getting the victory.
  bump_epoch(m->epoch);
  
  // they win
  mon->lose_election(epoch, from);
  
  // cancel my timer
  cancel_timer();	
}




void Elector::dispatch(Message *m)
{
  switch (m->get_type()) {

  case MSG_MON_ELECTION:
    {
      MMonElection *em = (MMonElection*)m;

      switch (em->op) {
      case MMonElection::OP_PROPOSE:
	handle_propose(em);
	return;
      }

      if (em->epoch < epoch) {
	dout(5) << "old epoch, dropping" << dendl;
	delete em;
	break;
      }

      switch (em->op) {
      case MMonElection::OP_ACK:
	handle_ack(em);
	return;
      case MMonElection::OP_VICTORY:
	handle_victory(em);
	return;
      default:
	assert(0);
      }
    }
    break;
    
  default: 
    assert(0);
  }
}




