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

#include "mdstypes.h"

#include "MDBalancer.h"
#include "MDS.h"
#include "MDSMap.h"
#include "CInode.h"
#include "CDir.h"
#include "MDCache.h"
#include "Migrator.h"

#include "include/Context.h"
#include "msg/Messenger.h"
#include "messages/MHeartbeat.h"

#include <vector>
#include <map>
using std::map;
using std::vector;

#include "config.h"

#define  dout(l)    if (l<=g_conf.debug_mds || l<=g_conf.debug_mds_balancer) *_dout << dbeginl << g_clock.now() << " mds" << mds->get_nodeid() << ".bal "

#define MIN_LOAD    50   //  ??
#define MIN_REEXPORT 5  // will automatically reexport
#define MIN_OFFLOAD 10   // point at which i stop trying, close enough



int MDBalancer::proc_message(Message *m)
{
  switch (m->get_type()) {

  case MSG_MDS_HEARTBEAT:
    handle_heartbeat((MHeartbeat*)m);
    break;
    
  default:
    dout(1) << " balancer unknown message " << m->get_type() << dendl;
    assert(0);
    break;
  }

  return 0;
}




void MDBalancer::tick()
{
  static int num_bal_times = g_conf.mds_bal_max;
  static utime_t first = g_clock.now();
  utime_t now = g_clock.now();
  utime_t elapsed = now;
  elapsed -= first;

  // sample?
  if ((double)now - (double)last_sample > g_conf.mds_bal_sample_interval) {
    dout(15) << "tick last_sample now " << now << dendl;
    last_sample = now;
  }

  // balance?
  if (last_heartbeat == utime_t()) last_heartbeat = now;
  if (true && 
      mds->get_nodeid() == 0 &&
      g_conf.mds_bal_interval > 0 &&
      (num_bal_times || 
       (g_conf.mds_bal_max_until >= 0 && 
	elapsed.sec() > g_conf.mds_bal_max_until)) && 
      mds->is_active() &&
      now.sec() - last_heartbeat.sec() >= g_conf.mds_bal_interval) {
    last_heartbeat = now;
    send_heartbeat();
    num_bal_times--;
  }
  
  // hash?
  if (true &&
      now.sec() - last_fragment.sec() > g_conf.mds_bal_fragment_interval) {
    last_fragment = now;
    do_fragmenting();
  }
}




class C_Bal_SendHeartbeat : public Context {
public:
  MDS *mds;
  C_Bal_SendHeartbeat(MDS *mds) {
    this->mds = mds;
  }
  virtual void finish(int f) {
    mds->balancer->send_heartbeat();
  }
};


double mds_load_t::mds_load() 
{
  switch(g_conf.mds_bal_mode) {
  case 0: 
    return 
      .8 * auth.meta_load() +
      .2 * all.meta_load() +
      req_rate +
      10.0 * queue_len;
    
  case 1:
    return req_rate + 10.0*queue_len;
    
  case 2:
    return cpu_load_avg;

  }
  assert(0);
  return 0;
}

mds_load_t MDBalancer::get_load()
{
  mds_load_t load;

  if (mds->mdcache->get_root()) {
    list<CDir*> ls;
    mds->mdcache->get_root()->get_dirfrags(ls);
    for (list<CDir*>::iterator p = ls.begin();
	 p != ls.end();
	 p++) {
      load.auth += (*p)->pop_auth_subtree_nested;
      load.all += (*p)->pop_nested;
    }
  } else {
    dout(20) << "get_load no root, no load" << dendl;
  }

  load.req_rate = mds->get_req_rate();
  load.queue_len = mds->messenger->get_dispatch_queue_len();

  ifstream cpu("/proc/loadavg");
  if (cpu.is_open()) 
    cpu >> load.cpu_load_avg;

  dout(15) << "get_load " << load << dendl;
  return load;
}

void MDBalancer::send_heartbeat()
{
  utime_t now = g_clock.now();
  if (!mds->mdcache->get_root()) {
    dout(5) << "no root on send_heartbeat" << dendl;
    mds->mdcache->open_root(new C_Bal_SendHeartbeat(mds));
    return;
  }

  mds_load.clear();
  if (mds->get_nodeid() == 0)
    beat_epoch++;

  // my load
  mds_load_t load = get_load();
  mds_load[ mds->get_nodeid() ] = load;

  // import_map -- how much do i import from whom
  map<int, float> import_map;
  set<CDir*> authsubs;
  mds->mdcache->get_auth_subtrees(authsubs);
  for (set<CDir*>::iterator it = authsubs.begin();
       it != authsubs.end();
       it++) {
    CDir *im = *it;
    int from = im->inode->authority().first;
    if (from == mds->get_nodeid()) continue;
    if (im->get_inode()->is_stray()) continue;
    import_map[from] += im->pop_auth_subtree.meta_load(now);
  }
  mds_import_map[ mds->get_nodeid() ] = import_map;
  
  
  dout(5) << "mds" << mds->get_nodeid() << " epoch " << beat_epoch << " load " << load << dendl;
  for (map<int, float>::iterator it = import_map.begin();
       it != import_map.end();
       it++) {
    dout(5) << "  import_map from " << it->first << " -> " << it->second << dendl;
  }

  
  set<int> up;
  mds->get_mds_map()->get_in_mds_set(up);
  for (set<int>::iterator p = up.begin(); p != up.end(); ++p) {
    if (*p == mds->get_nodeid()) continue;
    MHeartbeat *hb = new MHeartbeat(load, beat_epoch);
    hb->get_import_map() = import_map;
    mds->messenger->send_message(hb,
                                 mds->mdsmap->get_inst(*p));
  }
}

void MDBalancer::handle_heartbeat(MHeartbeat *m)
{
  dout(25) << "=== got heartbeat " << m->get_beat() << " from " << m->get_source().num() << " " << m->get_load() << dendl;
  
  if (!mds->is_active()) 
    return;

  if (!mds->mdcache->get_root()) {
    dout(10) << "opening root on handle_heartbeat" << dendl;
    mds->mdcache->open_root(new C_MDS_RetryMessage(mds, m));
    return;
  }

  int who = m->get_source().num();
  
  if (who == 0) {
    dout(20) << " from mds0, new epoch" << dendl;
    beat_epoch = m->get_beat();
    send_heartbeat();

    show_imports();
  }
  
  mds_load[ who ] = m->get_load();
  mds_import_map[ who ] = m->get_import_map();

  //dout(0) << "  load is " << load << " have " << mds_load.size() << dendl;
  
  unsigned cluster_size = mds->get_mds_map()->get_num_in_mds();
  if (mds_load.size() == cluster_size) {
    // let's go!
    //export_empties();  // no!
    do_rebalance(m->get_beat());
  }
  
  // done
  delete m;
}


void MDBalancer::export_empties() 
{
  dout(5) << "export_empties checking for empty imports" << dendl;

  for (map<CDir*,set<CDir*> >::iterator it = mds->mdcache->subtrees.begin();
       it != mds->mdcache->subtrees.end();
       it++) {
    CDir *dir = it->first;
    if (!dir->is_auth() ||
	dir->is_ambiguous_auth() ||
	dir->is_freezing() ||
	dir->is_frozen()) 
      continue;
    
    if (!dir->inode->is_root() && dir->get_size() == 0) 
      mds->mdcache->migrator->export_empty_import(dir);
  }
}



double MDBalancer::try_match(int ex, double& maxex, 
                             int im, double& maxim)
{
  if (maxex <= 0 || maxim <= 0) return 0.0;
  
  double howmuch = MIN(maxex, maxim);
  if (howmuch <= 0) return 0.0;
  
  dout(5) << "   - mds" << ex << " exports " << howmuch << " to mds" << im << dendl;
  
  if (ex == mds->get_nodeid())
    my_targets[im] += howmuch;
  
  exported[ex] += howmuch;
  imported[im] += howmuch;

  maxex -= howmuch;
  maxim -= howmuch;

  return howmuch;
}



void MDBalancer::do_fragmenting()
{
  if (split_queue.empty()) {
    dout(20) << "do_fragmenting has nothing to do" << dendl;
    return;
  }

  dout(0) << "do_fragmenting " << split_queue.size() << " dirs marked for possible splitting" << dendl;
  
  for (set<dirfrag_t>::iterator i = split_queue.begin();
       i != split_queue.end();
       i++) {
    CDir *dir = mds->mdcache->get_dirfrag(*i);
    if (!dir) continue;
    if (!dir->is_auth()) continue;

    dout(0) << "do_fragmenting splitting " << *dir << dendl;
    mds->mdcache->split_dir(dir, 4);
  }
  split_queue.clear();
}



void MDBalancer::do_rebalance(int beat)
{
  int cluster_size = mds->get_mds_map()->get_num_mds();
  int whoami = mds->get_nodeid();
  utime_t now = g_clock.now();

  dump_pop_map();

  // reset
  my_targets.clear();
  imported.clear();
  exported.clear();

  dout(5) << " do_rebalance: cluster loads are" << dendl;

  mds->mdcache->migrator->clear_export_queue();

  // rescale!  turn my mds_load back into meta_load units
  double load_fac = 1.0;
  if (mds_load[whoami].mds_load() > 0) {
    double metald = mds_load[whoami].auth.meta_load(now);
    double mdsld = mds_load[whoami].mds_load();
    load_fac = metald / mdsld;
    dout(7) << " load_fac is " << load_fac 
	     << " <- " << mds_load[whoami].auth << " " << metald
	    << " / " << mdsld
	    << dendl;
  }
  
  double total_load = 0;
  multimap<double,int> load_map;
  for (int i=0; i<cluster_size; i++) {
    double l = mds_load[i].mds_load() * load_fac;
    mds_meta_load[i] = l;

    if (whoami == 0)
      dout(-5) << "  mds" << i 
               << " " << mds_load[i] 
               << " = " << mds_load[i].mds_load() 
               << " ~ " << l << dendl;

    if (whoami == i) my_load = l;
    total_load += l;

    load_map.insert(pair<double,int>( l, i ));
  }

  // target load
  target_load = total_load / (double)cluster_size;
  dout(5) << "do_rebalance:  my load " << my_load 
          << "   target " << target_load 
          << "   total " << total_load 
          << dendl;
  
  // under or over?
  if (my_load < target_load * (1.0 + g_conf.mds_bal_min_rebalance)) {
    dout(5) << "  i am underloaded or barely overloaded, doing nothing." << dendl;
    last_epoch_under = beat_epoch;
    show_imports();
    return;
  }
  
  last_epoch_over = beat_epoch;

  // am i over long enough?
  if (last_epoch_under && beat_epoch - last_epoch_under < 2) {
    dout(5) << "  i am overloaded, but only for " << (beat_epoch - last_epoch_under) << " epochs" << dendl;
    return;
  }

  dout(5) << "  i am sufficiently overloaded" << dendl;


  // first separate exporters and importers
  multimap<double,int> importers;
  multimap<double,int> exporters;
  set<int>             importer_set;
  set<int>             exporter_set;
  
  for (multimap<double,int>::iterator it = load_map.begin();
       it != load_map.end();
       it++) {
    if (it->first < target_load) {
      dout(15) << "   mds" << it->second << " is importer" << dendl;
      importers.insert(pair<double,int>(it->first,it->second));
      importer_set.insert(it->second);
    } else {
      dout(15) << "   mds" << it->second << " is exporter" << dendl;
      exporters.insert(pair<double,int>(it->first,it->second));
      exporter_set.insert(it->second);
    }
  }


  // determine load transfer mapping

  if (true) {
    // analyze import_map; do any matches i can

    dout(15) << "  matching exporters to import sources" << dendl;

    // big -> small exporters
    for (multimap<double,int>::reverse_iterator ex = exporters.rbegin();
         ex != exporters.rend();
         ex++) {
      double maxex = get_maxex(ex->second);
      if (maxex <= .001) continue;
      
      // check importers. for now, just in arbitrary order (no intelligent matching).
      for (map<int, float>::iterator im = mds_import_map[ex->second].begin();
           im != mds_import_map[ex->second].end();
           im++) {
        double maxim = get_maxim(im->first);
        if (maxim <= .001) continue;
        try_match(ex->second, maxex,
                  im->first, maxim);
        if (maxex <= .001) break;;
      }
    }
  }


  if (1) {
    if (beat % 2 == 1) {
      // old way
      dout(15) << "  matching big exporters to big importers" << dendl;
      // big exporters to big importers
      multimap<double,int>::reverse_iterator ex = exporters.rbegin();
      multimap<double,int>::iterator im = importers.begin();
      while (ex != exporters.rend() &&
             im != importers.end()) {
        double maxex = get_maxex(ex->second);
        double maxim = get_maxim(im->second);
        if (maxex < .001 || maxim < .001) break;
        try_match(ex->second, maxex,
                  im->second, maxim);
        if (maxex <= .001) ex++;
        if (maxim <= .001) im++;
      }
    } else {
      // new way
      dout(15) << "  matching small exporters to big importers" << dendl;
      // small exporters to big importers
      multimap<double,int>::iterator ex = exporters.begin();
      multimap<double,int>::iterator im = importers.begin();
      while (ex != exporters.end() &&
             im != importers.end()) {
        double maxex = get_maxex(ex->second);
        double maxim = get_maxim(im->second);
        if (maxex < .001 || maxim < .001) break;
        try_match(ex->second, maxex,
                  im->second, maxim);
        if (maxex <= .001) ex++;
        if (maxim <= .001) im++;
      }
    }
  }



  // make a sorted list of my imports
  map<double,CDir*>    import_pop_map;
  multimap<int,CDir*>  import_from_map;
  set<CDir*> fullauthsubs;

  mds->mdcache->get_fullauth_subtrees(fullauthsubs);
  for (set<CDir*>::iterator it = fullauthsubs.begin();
       it != fullauthsubs.end();
       it++) {
    CDir *im = *it;
    if (im->get_inode()->is_stray()) continue;

    double pop = im->pop_auth_subtree.meta_load(now);
    if (g_conf.mds_bal_idle_threshold > 0 &&
	pop < g_conf.mds_bal_idle_threshold &&
        im->inode != mds->mdcache->get_root() &&
	im->inode->authority().first != mds->get_nodeid()) {
      dout(-5) << " exporting idle (" << pop << ") import " << *im
               << " back to mds" << im->inode->authority().first
               << dendl;
      mds->mdcache->migrator->export_dir_nicely(im, im->inode->authority().first);
      continue;
    }

    import_pop_map[ pop ] = im;
    int from = im->inode->authority().first;
    dout(15) << "  map: i imported " << *im << " from " << from << dendl;
    import_from_map.insert(pair<int,CDir*>(from, im));
  }
  


  // do my exports!
  set<CDir*> already_exporting;
  double total_sent = 0;
  double total_goal = 0;

  for (map<int,double>::iterator it = my_targets.begin();
       it != my_targets.end();
       it++) {

    /*
    double fac = 1.0;
    if (false && total_goal > 0 && total_sent > 0) {
      fac = total_goal / total_sent;
      dout(-5) << " total sent is " << total_sent << " / " << total_goal << " -> fac 1/ " << fac << dendl;
      if (fac > 1.0) fac = 1.0;
    }
    fac = .9 - .4 * ((float)g_conf.num_mds / 128.0);  // hack magic fixme
    */
    
    int target = (*it).first;
    double amount = (*it).second;
    total_goal += amount;

    if (amount < MIN_OFFLOAD) continue;
    if (amount / target_load < .2) continue;

    dout(5) << "want to send " << amount << " to mds" << target 
      //<< " .. " << (*it).second << " * " << load_fac 
            << " -> " << amount
            << dendl;//" .. fudge is " << fudge << dendl;
    double have = 0;

    
    show_imports();

    // search imports from target
    if (import_from_map.count(target)) {
      dout(5) << " aha, looking through imports from target mds" << target << dendl;
      pair<multimap<int,CDir*>::iterator, multimap<int,CDir*>::iterator> p =
        import_from_map.equal_range(target);
      while (p.first != p.second) {
        CDir *dir = (*p.first).second;
        dout(5) << "considering " << *dir << " from " << (*p.first).first << dendl;
        multimap<int,CDir*>::iterator plast = p.first++;
        
        if (dir->inode->is_root()) continue;
        if (dir->is_freezing() || dir->is_frozen()) continue;  // export pbly already in progress
        double pop = dir->pop_auth_subtree.meta_load(now);
        assert(dir->inode->authority().first == target);  // cuz that's how i put it in the map, dummy
        
        if (pop <= amount-have) {
          dout(-5) << "reexporting " << *dir 
                   << " pop " << pop 
                   << " back to mds" << target << dendl;
          mds->mdcache->migrator->export_dir_nicely(dir, target);
          have += pop;
          import_from_map.erase(plast);
          import_pop_map.erase(pop);
        } else {
          dout(5) << "can't reexport " << *dir << ", too big " << pop << dendl;
        }
        if (amount-have < MIN_OFFLOAD) break;
      }
    }
    if (amount-have < MIN_OFFLOAD) {
      total_sent += have;
      continue;
    }
    
    // any other imports
    if (false)
    for (map<double,CDir*>::iterator import = import_pop_map.begin();
         import != import_pop_map.end();
         import++) {
      CDir *imp = (*import).second;
      if (imp->inode->is_root()) continue;
      
      double pop = (*import).first;
      if (pop < amount-have || pop < MIN_REEXPORT) {
        dout(-5) << "reexporting " << *imp 
                 << " pop " << pop 
                 << " back to mds" << imp->inode->authority()
                 << dendl;
        have += pop;
        mds->mdcache->migrator->export_dir_nicely(imp, imp->inode->authority().first);
      }
      if (amount-have < MIN_OFFLOAD) break;
    }
    if (amount-have < MIN_OFFLOAD) {
      //fudge = amount-have;
      total_sent += have;
      continue;
    }

    // okay, search for fragments of my workload
    set<CDir*> candidates;
    mds->mdcache->get_fullauth_subtrees(candidates);

    list<CDir*> exports;
    
    for (set<CDir*>::iterator pot = candidates.begin();
         pot != candidates.end();
         pot++) {
      if ((*pot)->get_inode()->is_stray()) continue;
      find_exports(*pot, amount, exports, have, already_exporting, now);
      if (have > amount-MIN_OFFLOAD) 
        break;
    }
    //fudge = amount - have;
    total_sent += have;
    
    for (list<CDir*>::iterator it = exports.begin(); it != exports.end(); it++) {
      dout(-5) << "   - exporting " 
	       << (*it)->pop_auth_subtree
	       << " "
	       << (*it)->pop_auth_subtree.meta_load(now) 
	       << " to mds" << target 
               << " " << **it 
               << dendl;
      mds->mdcache->migrator->export_dir_nicely(*it, target);
    }
  }

  dout(5) << "rebalance done" << dendl;
  show_imports();
  
}



void MDBalancer::find_exports(CDir *dir, 
                              double amount, 
                              list<CDir*>& exports, 
                              double& have,
                              set<CDir*>& already_exporting,
			      utime_t now)
{
  double need = amount - have;
  if (need < amount * g_conf.mds_bal_min_start)
    return;   // good enough!
  double needmax = need * g_conf.mds_bal_need_max;
  double needmin = need * g_conf.mds_bal_need_min;
  double midchunk = need * g_conf.mds_bal_midchunk;
  double minchunk = need * g_conf.mds_bal_minchunk;

  list<CDir*> bigger_rep, bigger_unrep;
  multimap<double, CDir*> smaller;

  double dir_pop = dir->pop_auth_subtree.meta_load(now);
  dout(7) << " find_exports in " << dir_pop << " " << *dir << " need " << need << " (" << needmin << " - " << needmax << ")" << dendl;

  double subdir_sum = 0;
  for (CDir::map_t::iterator it = dir->begin();
       it != dir->end();
       it++) {
    CInode *in = it->second->get_inode();
    if (!in) continue;
    if (!in->is_dir()) continue;
    
    list<CDir*> dfls;
    in->get_dirfrags(dfls);
    for (list<CDir*>::iterator p = dfls.begin();
	 p != dfls.end();
	 ++p) {
      CDir *subdir = *p;
      if (!subdir->is_auth()) continue;
      if (already_exporting.count(subdir)) continue;

      if (subdir->is_frozen()) continue;  // can't export this right now!
      
      // how popular?
      double pop = subdir->pop_auth_subtree.meta_load(now);
      subdir_sum += pop;
      dout(15) << "   subdir pop " << pop << " " << *subdir << dendl;

      if (pop < minchunk) continue;
      
      // lucky find?
      if (pop > needmin && pop < needmax) {
	exports.push_back(subdir);
	already_exporting.insert(subdir);
	have += pop;
	return;
      }
      
      if (pop > need) {
	if (subdir->is_rep())
	  bigger_rep.push_back(subdir);
	else
	  bigger_unrep.push_back(subdir);
      } else
	smaller.insert(pair<double,CDir*>(pop, subdir));
    }
  }
  dout(15) << "   sum " << subdir_sum << " / " << dir_pop << dendl;

  // grab some sufficiently big small items
  multimap<double,CDir*>::reverse_iterator it;
  for (it = smaller.rbegin();
       it != smaller.rend();
       it++) {

    if ((*it).first < midchunk)
      break;  // try later
    
    dout(7) << "   taking smaller " << *(*it).second << dendl;
    
    exports.push_back((*it).second);
    already_exporting.insert((*it).second);
    have += (*it).first;
    if (have > needmin)
      return;
  }
  
  // apprently not enough; drill deeper into the hierarchy (if non-replicated)
  for (list<CDir*>::iterator it = bigger_unrep.begin();
       it != bigger_unrep.end();
       it++) {
    dout(15) << "   descending into " << **it << dendl;
    find_exports(*it, amount, exports, have, already_exporting, now);
    if (have > needmin)
      return;
  }

  // ok fine, use smaller bits
  for (;
       it != smaller.rend();
       it++) {
    dout(7) << "   taking (much) smaller " << it->first << " " << *(*it).second << dendl;

    exports.push_back((*it).second);
    already_exporting.insert((*it).second);
    have += (*it).first;
    if (have > needmin)
      return;
  }

  // ok fine, drill into replicated dirs
  for (list<CDir*>::iterator it = bigger_rep.begin();
       it != bigger_rep.end();
       it++) {
    dout(7) << "   descending into replicated " << **it << dendl;
    find_exports(*it, amount, exports, have, already_exporting, now);
    if (have > needmin)
      return;
  }

}




void MDBalancer::hit_inode(utime_t now, CInode *in, int type, int who)
{
  // hit inode
  in->pop.get(type).hit(now);

  if (in->get_parent_dn())
    hit_dir(now, in->get_parent_dn()->get_dir(), type, who);
}
/*
  // hit me
  in->popularity[MDS_POP_JUSTME].pop[type].hit(now);
  in->popularity[MDS_POP_NESTED].pop[type].hit(now);
  if (in->is_auth()) {
    in->popularity[MDS_POP_CURDOM].pop[type].hit(now);
    in->popularity[MDS_POP_ANYDOM].pop[type].hit(now);

    dout(20) << "hit_inode " << type << " pop "
	     << in->popularity[MDS_POP_JUSTME].pop[type].get(now) << " me, "
	     << in->popularity[MDS_POP_NESTED].pop[type].get(now) << " nested, "
	     << in->popularity[MDS_POP_CURDOM].pop[type].get(now) << " curdom, " 
	     << in->popularity[MDS_POP_CURDOM].pop[type].get(now) << " anydom" 
	     << " on " << *in
	     << dendl;
  } else {
    dout(20) << "hit_inode " << type << " pop "
	     << in->popularity[MDS_POP_JUSTME].pop[type].get(now) << " me, "
	     << in->popularity[MDS_POP_NESTED].pop[type].get(now) << " nested, "
      	     << " on " << *in
	     << dendl;
  }

  // hit auth up to import
  CDir *dir = in->get_parent_dir();
  if (dir) hit_dir(now, dir, type);
*/


void MDBalancer::hit_dir(utime_t now, CDir *dir, int type, int who, double amount) 
{
  // hit me
  double v = dir->pop_me.get(type).hit(now, amount);
  
  //if (dir->ino() == inodeno_t(0x10000000000))
  //dout(0) << "hit_dir " << type << " pop " << v << " in " << *dir << dendl;

  // hit modify counter, if this was a modify
  if (g_conf.num_mds > 2 &&             // FIXME >2 thing
      !dir->inode->is_root() &&        // not root (for now at least)
      dir->is_auth() &&
      
      ((g_conf.mds_bal_split_size > 0 &&
	dir->get_size() > (unsigned)g_conf.mds_bal_split_size) ||
       (v > g_conf.mds_bal_split_rd && type == META_POP_IRD) ||
       (v > g_conf.mds_bal_split_wr && type == META_POP_IWR)) &&
      split_queue.count(dir->dirfrag()) == 0) {
    dout(0) << "hit_dir " << type << " pop is " << v << ", putting in split_queue: " << *dir << dendl;
    split_queue.insert(dir->dirfrag());
  }
  
  // replicate?
  if (type == META_POP_IRD && who >= 0) {
    dir->pop_spread.hit(now, who);
  }

  double rd_adj = 0;
  if (type == META_POP_IRD &&
      dir->last_popularity_sample < last_sample) {
    float dir_pop = dir->pop_auth_subtree.get(type).get(now);    // hmm??
    dir->last_popularity_sample = last_sample;
    float pop_sp = dir->pop_spread.get(now);
    dir_pop += pop_sp * 10;

    //if (dir->ino() == inodeno_t(0x10000000002))
    if (pop_sp > 0) {
      dout(20) << "hit_dir " << type << " pop " << dir_pop << " spread " << pop_sp 
	      << " " << dir->pop_spread.last[0]
	      << " " << dir->pop_spread.last[1]
	      << " " << dir->pop_spread.last[2]
	      << " " << dir->pop_spread.last[3]
	      << " in " << *dir << dendl;
    }
    
    if (dir->is_auth() && !dir->is_ambiguous_auth()) {
      if (!dir->is_rep() &&
	  dir_pop >= g_conf.mds_bal_replicate_threshold) {
	// replicate
	float rdp = dir->pop_me.get(META_POP_IRD).get(now);
	rd_adj = rdp / mds->get_mds_map()->get_num_mds() - rdp; 
	rd_adj /= 2.0;  // temper somewhat
	
	dout(0) << "replicating dir " << *dir << " pop " << dir_pop << " .. rdp " << rdp << " adj " << rd_adj << dendl;
	
	dir->dir_rep = CDir::REP_ALL;
	mds->mdcache->send_dir_updates(dir, true);
	
	// fixme this should adjust the whole pop hierarchy
	dir->pop_me.get(META_POP_IRD).adjust(rd_adj);
	dir->pop_auth_subtree.get(META_POP_IRD).adjust(rd_adj);
      }
      
      if (dir->ino() != 1 &&
	  dir->is_rep() &&
	  dir_pop < g_conf.mds_bal_unreplicate_threshold) {
	// unreplicate
	dout(0) << "unreplicating dir " << *dir << " pop " << dir_pop << dendl;
	
	dir->dir_rep = CDir::REP_NONE;
	mds->mdcache->send_dir_updates(dir);
      }
    }
  }

  // adjust ancestors
  bool hit_subtree = dir->is_auth();         // current auth subtree (if any)
  bool hit_subtree_nested = dir->is_auth();  // all nested auth subtrees

  while (1) {
    dir->pop_nested.get(type).hit(now, amount);
    if (rd_adj != 0.0) 
      dir->pop_nested.get(META_POP_IRD).adjust(now, rd_adj);
    
    if (hit_subtree) {
      dir->pop_auth_subtree.get(type).hit(now, amount);
      if (rd_adj != 0.0) 
	dir->pop_auth_subtree.get(META_POP_IRD).adjust(now, rd_adj);
    }

    if (hit_subtree_nested) {
      dir->pop_auth_subtree_nested.get(type).hit(now, amount);
      if (rd_adj != 0.0) 
	dir->pop_auth_subtree_nested.get(META_POP_IRD).adjust(now, rd_adj);
    }  
    
    if (dir->is_subtree_root()) 
      hit_subtree = false;                // end of auth domain, stop hitting auth counters.

    if (dir->inode->get_parent_dn() == 0) break;
    dir = dir->inode->get_parent_dn()->get_dir();
  }
}


/*
 * subtract off an exported chunk.
 *  this excludes *dir itself (encode_export_dir should have take care of that)
 *  we _just_ do the parents' nested counters.
 *
 * NOTE: call me _after_ forcing *dir into a subtree root,
 *       but _before_ doing the encode_export_dirs.
 */
void MDBalancer::subtract_export(CDir *dir)
{
  dirfrag_load_vec_t subload = dir->pop_auth_subtree;

  while (true) {
    dir = dir->inode->get_parent_dir();
    if (!dir) break;
    
    dir->pop_nested -= subload;
    dir->pop_auth_subtree_nested -= subload;
  }
}
    

void MDBalancer::add_import(CDir *dir)
{
  dirfrag_load_vec_t subload = dir->pop_auth_subtree;
  
  while (true) {
    dir = dir->inode->get_parent_dir();
    if (!dir) break;
    
    dir->pop_nested += subload;
    dir->pop_auth_subtree_nested += subload;
  } 
}






void MDBalancer::show_imports(bool external)
{
  mds->mdcache->show_subtrees();
}


void MDBalancer::dump_pop_map()
{
  return; // this is dumb


  char fn[20];
  sprintf(fn, "popdump.%d.mds%d", beat_epoch, mds->get_nodeid());

  dout(1) << "dump_pop_map to " << fn << dendl;

  ofstream myfile;
  myfile.open(fn);

  list<CInode*> iq;
  if (mds->mdcache->root)
    iq.push_back(mds->mdcache->root);

  utime_t now = g_clock.now();
  while (!iq.empty()) {
    CInode *in = iq.front();
    iq.pop_front();
    
    // pop stats
    /*for (int a=0; a<MDS_NPOP; a++) 
      for (int b=0; b<META_NPOP; b++)
	myfile << in->popularity[a].pop[b].get(now) << "\t";
    */

    // recurse, depth-first.
    if (in->is_dir()) {

      list<CDir*> dirs;
      in->get_dirfrags(dirs);
      for (list<CDir*>::iterator p = dirs.begin();
	   p != dirs.end();
	   ++p) {
	CDir *dir = *p;

	myfile << (int)dir->pop_me.meta_load(now) << "\t";
	myfile << (int)dir->pop_nested.meta_load(now) << "\t";
	myfile << (int)dir->pop_auth_subtree.meta_load(now) << "\t";
	myfile << (int)dir->pop_auth_subtree_nested.meta_load(now) << "\t";

	// filename last
	string p;
	in->make_path_string(p);
	myfile << "." << p;
	if (dir->get_frag() != frag_t()) 
	  myfile << "___" << (unsigned)dir->get_frag();
	myfile << std::endl; //"/" << dir->get_frag() << dendl;
	
	// add contents
	for (CDir::map_t::iterator q = dir->items.begin();
	     q != dir->items.end();
	     q++) 
	  if (q->second->is_primary())
	    iq.push_front(q->second->get_inode());
      }
    }
    
  }

  myfile.close();
}



/*  replicate?

      float dir_pop = dir->get_popularity();
      
      if (dir->is_auth()) {
        if (!dir->is_rep() &&
            dir_pop >= g_conf.mds_bal_replicate_threshold) {
          // replicate
          dout(5) << "replicating dir " << *in << " pop " << dir_pop << dendl;
          
          dir->dir_rep = CDIR_REP_ALL;
          mds->mdcache->send_dir_updates(dir);
        }
        
        if (dir->is_rep() &&
            dir_pop < g_conf.mds_bal_unreplicate_threshold) {
          // unreplicate
          dout(5) << "unreplicating dir " << *in << " pop " << dir_pop << dendl;
          
          dir->dir_rep = CDIR_REP_NONE;
          mds->mdcache->send_dir_updates(dir);
        }
      }

*/
