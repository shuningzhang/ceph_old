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

#include "MDS.h"
#include "MDCache.h"
#include "CInode.h"
#include "CDir.h"
#include "CDentry.h"
#include "Migrator.h"
#include "Locker.h"
#include "Migrator.h"
#include "Server.h"

#include "MDBalancer.h"
#include "MDLog.h"
#include "MDSMap.h"

#include "include/filepath.h"

#include "events/EString.h"
#include "events/EExport.h"
#include "events/EImportStart.h"
#include "events/EImportFinish.h"
#include "events/ESessions.h"

#include "msg/Messenger.h"

#include "messages/MClientFileCaps.h"

#include "messages/MExportDirDiscover.h"
#include "messages/MExportDirDiscoverAck.h"
#include "messages/MExportDirCancel.h"
#include "messages/MExportDirPrep.h"
#include "messages/MExportDirPrepAck.h"
#include "messages/MExportDir.h"
#include "messages/MExportDirAck.h"
#include "messages/MExportDirNotify.h"
#include "messages/MExportDirNotifyAck.h"
#include "messages/MExportDirFinish.h"

#include "messages/MExportCaps.h"
#include "messages/MExportCapsAck.h"





#include "config.h"

#define  dout(l)    if (l<=g_conf.debug || l <= g_conf.debug_mds || l <= g_conf.debug_mds_migrator) *_dout << dbeginl << g_clock.now() << " mds" << mds->get_nodeid() << ".migrator "



void Migrator::dispatch(Message *m)
{
  switch (m->get_type()) {
    // import
  case MSG_MDS_EXPORTDIRDISCOVER:
    handle_export_discover((MExportDirDiscover*)m);
    break;
  case MSG_MDS_EXPORTDIRPREP:
    handle_export_prep((MExportDirPrep*)m);
    break;
  case MSG_MDS_EXPORTDIR:
    handle_export_dir((MExportDir*)m);
    break;
  case MSG_MDS_EXPORTDIRFINISH:
    handle_export_finish((MExportDirFinish*)m);
    break;
  case MSG_MDS_EXPORTDIRCANCEL:
    handle_export_cancel((MExportDirCancel*)m);
    break;

    // export 
  case MSG_MDS_EXPORTDIRDISCOVERACK:
    handle_export_discover_ack((MExportDirDiscoverAck*)m);
    break;
  case MSG_MDS_EXPORTDIRPREPACK:
    handle_export_prep_ack((MExportDirPrepAck*)m);
    break;
  case MSG_MDS_EXPORTDIRACK:
    handle_export_ack((MExportDirAck*)m);
    break;
  case MSG_MDS_EXPORTDIRNOTIFYACK:
    handle_export_notify_ack((MExportDirNotifyAck*)m);
    break;    

    // export 3rd party (dir_auth adjustments)
  case MSG_MDS_EXPORTDIRNOTIFY:
    handle_export_notify((MExportDirNotify*)m);
    break;

    // caps
  case MSG_MDS_EXPORTCAPS:
    handle_export_caps((MExportCaps*)m);
    break;
  case MSG_MDS_EXPORTCAPSACK:
    handle_export_caps_ack((MExportCapsAck*)m);
    break;

  default:
    assert(0);
  }
}


class C_MDC_EmptyImport : public Context {
  Migrator *mig;
  CDir *dir;
public:
  C_MDC_EmptyImport(Migrator *m, CDir *d) : mig(m), dir(d) {}
  void finish(int r) {
    mig->export_empty_import(dir);
  }
};


void Migrator::export_empty_import(CDir *dir)
{
  dout(7) << "export_empty_import " << *dir << dendl;
  assert(dir->is_subtree_root());

  if (dir->inode->is_auth()) {
    dout(7) << " inode is auth" << dendl;
    return;
  }
  if (!dir->is_auth()) {
    dout(7) << " not auth" << dendl;
    return;
  }
  if (dir->is_freezing() || dir->is_frozen()) {
    dout(7) << " freezing or frozen" << dendl;
    return;
  }
  if (dir->get_size() > 0) {
    dout(7) << " not actually empty" << dendl;
    return;
  }
  if (dir->inode->is_root()) {
    dout(7) << " root" << dendl;
    return;
  }
  
  int dest = dir->inode->authority().first;
  //if (mds->is_shutting_down()) dest = 0;  // this is more efficient.
  
  dout(7) << " really empty, exporting to " << dest << dendl;
  assert (dest != mds->get_nodeid());
  
  dout(7) << "exporting to mds" << dest 
           << " empty import " << *dir << dendl;
  export_dir( dir, dest );
}




// ==========================================================
// mds failure handling

void Migrator::handle_mds_failure_or_stop(int who)
{
  dout(5) << "handle_mds_failure_or_stop mds" << who << dendl;

  // check my exports
  map<CDir*,int>::iterator p = export_state.begin();
  while (p != export_state.end()) {
    map<CDir*,int>::iterator next = p;
    next++;
    CDir *dir = p->first;
    
    // abort exports:
    //  - that are going to the failed node
    //  - that aren't frozen yet (to avoid auth_pin deadlock)
    if (export_peer[dir] == who ||
	p->second == EXPORT_DISCOVERING || p->second == EXPORT_FREEZING) { 
      // the guy i'm exporting to failed, or we're just freezing.
      dout(10) << "cleaning up export state " << p->second << " of " << *dir << dendl;
      
      switch (p->second) {
      case EXPORT_DISCOVERING:
	dout(10) << "export state=discovering : canceling freeze and removing auth_pin" << dendl;
	dir->unfreeze_tree();  // cancel the freeze
	dir->auth_unpin();
	export_state.erase(dir); // clean up
	dir->state_clear(CDir::STATE_EXPORTING);
	if (export_peer[dir] != who) // tell them.
	  mds->send_message_mds(new MExportDirCancel(dir->dirfrag()), export_peer[dir]);
	break;
	
      case EXPORT_FREEZING:
	dout(10) << "export state=freezing : canceling freeze" << dendl;
	dir->unfreeze_tree();  // cancel the freeze
	export_state.erase(dir); // clean up
	dir->state_clear(CDir::STATE_EXPORTING);
	if (export_peer[dir] != who) // tell them.
	  mds->send_message_mds(new MExportDirCancel(dir->dirfrag()), export_peer[dir]);
	break;

	// NOTE: state order reversal, warning comes after loggingstart+prepping
      case EXPORT_WARNING:
	dout(10) << "export state=warning : unpinning bounds, unfreezing, notifying" << dendl;
	// fall-thru

	//case EXPORT_LOGGINGSTART:
      case EXPORT_PREPPING:
	if (p->second != EXPORT_WARNING) 
	  dout(10) << "export state=loggingstart|prepping : unpinning bounds, unfreezing" << dendl;
	{
	  // unpin bounds
	  set<CDir*> bounds;
	  cache->get_subtree_bounds(dir, bounds);
	  for (set<CDir*>::iterator p = bounds.begin();
	       p != bounds.end();
	       ++p) {
	    CDir *bd = *p;
	    bd->put(CDir::PIN_EXPORTBOUND);
	    bd->state_clear(CDir::STATE_EXPORTBOUND);
	  }
	}
	dir->unfreeze_tree();
	cache->adjust_subtree_auth(dir, mds->get_nodeid());
	cache->try_subtree_merge(dir);
	export_state.erase(dir); // clean up
	dir->state_clear(CDir::STATE_EXPORTING);
	break;
	
      case EXPORT_EXPORTING:
	dout(10) << "export state=exporting : reversing, and unfreezing" << dendl;
	export_reverse(dir);
	export_state.erase(dir); // clean up
	dir->state_clear(CDir::STATE_EXPORTING);
	break;

      case EXPORT_LOGGINGFINISH:
      case EXPORT_NOTIFYING:
	dout(10) << "export state=loggingfinish|notifying : ignoring dest failure, we were successful." << dendl;
	// leave export_state, don't clean up now.
	break;

      default:
	assert(0);
      }

      // finish clean-up?
      if (export_state.count(dir) == 0) {
	export_peer.erase(dir);
	export_warning_ack_waiting.erase(dir);
	export_notify_ack_waiting.erase(dir);
	
	// unpin the path
	vector<CDentry*> trace;
	cache->make_trace(trace, dir->inode);
	mds->locker->dentry_anon_rdlock_trace_finish(trace);
	
	// wake up any waiters
	mds->queue_waiters(export_finish_waiters[dir]);
	export_finish_waiters.erase(dir);
	
	// send pending import_maps?  (these need to go out when all exports have finished.)
	cache->maybe_send_pending_resolves();

	cache->show_subtrees();

	maybe_do_queued_export();	
      }
    } else {
      // bystander failed.
      if (export_warning_ack_waiting.count(dir) &&
	  export_warning_ack_waiting[dir].count(who)) {
	export_warning_ack_waiting[dir].erase(who);
	export_notify_ack_waiting[dir].erase(who);   // they won't get a notify either.
	if (p->second == EXPORT_WARNING) {
	  // exporter waiting for warning acks, let's fake theirs.
	  dout(10) << "faking export_warning_ack from mds" << who
		   << " on " << *dir << " to mds" << export_peer[dir] 
		   << dendl;
	  if (export_warning_ack_waiting[dir].empty()) 
	    export_go(dir);
	}
      }
      if (export_notify_ack_waiting.count(dir) &&
	  export_notify_ack_waiting[dir].count(who)) {
	export_notify_ack_waiting[dir].erase(who);
	if (p->second == EXPORT_NOTIFYING) {
	  // exporter is waiting for notify acks, fake it
	  dout(10) << "faking export_notify_ack from mds" << who
		   << " on " << *dir << " to mds" << export_peer[dir] 
		   << dendl;
	  if (export_notify_ack_waiting[dir].empty()) 
	    export_finish(dir);
	}
      }
    }
    
    // next!
    p = next;
  }


  // check my imports
  map<dirfrag_t,int>::iterator q = import_state.begin();
  while (q != import_state.end()) {
    map<dirfrag_t,int>::iterator next = q;
    next++;
    dirfrag_t df = q->first;
    CInode *diri = mds->mdcache->get_inode(df.ino);
    CDir *dir = mds->mdcache->get_dirfrag(df);

    if (import_peer[df] == who) {
      switch (q->second) {
      case IMPORT_DISCOVERING:
	dout(10) << "import state=discovering : clearing state" << dendl;
	import_state.erase(df);
	import_peer.erase(df);
	break;

      case IMPORT_DISCOVERED:
	dout(10) << "import state=discovered : unpinning inode " << *diri << dendl;
	assert(diri);
	// unpin base
	diri->put(CInode::PIN_IMPORTING);
	import_state.erase(df);
	import_peer.erase(df);
	break;

      case IMPORT_PREPPING:
	if (q->second == IMPORT_PREPPING) {
	  dout(10) << "import state=prepping : unpinning base+bounds " << *dir << dendl;
	}
	assert(dir);
	{
	  set<CDir*> bounds;
	  cache->map_dirfrag_set(import_bound_ls[dir], bounds);
	  import_remove_pins(dir, bounds);
	  import_reverse_final(dir);
	}
	break;

      case IMPORT_PREPPED:
	dout(10) << "import state=prepped : unpinning base+bounds, unfreezing " << *dir << dendl;
	assert(dir);
	{
	  set<CDir*> bounds;
	  cache->get_subtree_bounds(dir, bounds);
	  import_remove_pins(dir, bounds);
	  
	  // adjust auth back to me
	  cache->adjust_subtree_auth(dir, import_peer[df]);
	  cache->try_subtree_merge(dir);
	  
	  // bystanders?
	  if (import_bystanders[dir].empty()) {
	    import_reverse_unfreeze(dir);
	  } else {
	    // notify them; wait in aborting state
	    import_notify_abort(dir, bounds);
	    import_state[df] = IMPORT_ABORTING;
	  }
	}
	break;

      case IMPORT_LOGGINGSTART:
	dout(10) << "import state=loggingstart : reversing import on " << *dir << dendl;
	import_reverse(dir);
	break;

      case IMPORT_ACKING:
	// hrm.  make this an ambiguous import, and wait for exporter recovery to disambiguate
	dout(10) << "import state=acking : noting ambiguous import " << *dir << dendl;
	{
	  set<CDir*> bounds;
	  cache->get_subtree_bounds(dir, bounds);
	  cache->add_ambiguous_import(dir, bounds);
	}
	break;
	
      case IMPORT_ABORTING:
	dout(10) << "import state=aborting : ignoring repeat failure " << *dir << dendl;
	break;
      }
    } else {
      if (q->second == IMPORT_ABORTING &&
	  import_bystanders[dir].count(who)) {
	dout(10) << "faking export_notify_ack from mds" << who
		 << " on aborting import " << *dir << " from mds" << import_peer[df] 
		 << dendl;
	import_bystanders[dir].erase(who);
	if (import_bystanders[dir].empty()) {
	  import_bystanders.erase(dir);
	  import_reverse_unfreeze(dir);
	}
      }
    }

    // next!
    q = next;
  }
}



void Migrator::show_importing()
{  
  dout(10) << "show_importing" << dendl;
  for (map<dirfrag_t,int>::iterator p = import_state.begin();
       p != import_state.end();
       p++) {
    CDir *dir = mds->mdcache->get_dirfrag(p->first);
    if (dir) {
      dout(10) << " importing from " << import_peer[p->first]
	       << ": (" << p->second << ") " << get_import_statename(p->second) 
	       << " " << p->first
	       << " " << *dir
	       << dendl;
    } else {
      dout(10) << " importing from " << import_peer[p->first]
	       << ": (" << p->second << ") " << get_import_statename(p->second) 
	       << " " << p->first 
	       << dendl;
    }
  }
}

void Migrator::show_exporting() 
{
  dout(10) << "show_exporting" << dendl;
  for (map<CDir*,int>::iterator p = export_state.begin();
       p != export_state.end();
       p++) 
    dout(10) << " exporting to " << export_peer[p->first]
	     << ": (" << p->second << ") " << get_export_statename(p->second) 
	     << " " << p->first->dirfrag()
	     << " " << *p->first
	     << dendl;
}



void Migrator::audit()
{
  if (g_conf.debug_mds < 5) return;  // hrm.

  // import_state
  show_importing();
  for (map<dirfrag_t,int>::iterator p = import_state.begin();
       p != import_state.end();
       p++) {
    if (p->second == IMPORT_DISCOVERING) 
      continue;
    if (p->second == IMPORT_DISCOVERED) {
      CInode *in = cache->get_inode(p->first.ino);
      assert(in);
      continue;
    }
    CDir *dir = cache->get_dirfrag(p->first);
    assert(dir);
    if (p->second == IMPORT_PREPPING) 
      continue;
    assert(dir->is_ambiguous_dir_auth());
    assert(dir->authority().first  == mds->get_nodeid() ||
	   dir->authority().second == mds->get_nodeid());
  }

  // export_state
  show_exporting();
  for (map<CDir*,int>::iterator p = export_state.begin();
       p != export_state.end();
       p++) {
    CDir *dir = p->first;
    if (p->second == EXPORT_DISCOVERING ||
	p->second == EXPORT_FREEZING) continue;
    assert(dir->is_ambiguous_dir_auth());
    assert(dir->authority().first  == mds->get_nodeid() ||
	   dir->authority().second == mds->get_nodeid());
  }

  // ambiguous+me subtrees should be importing|exporting

  // write me
}





// ==========================================================
// EXPORT

void Migrator::export_dir_nicely(CDir *dir, int dest)
{
  // enqueue
  dout(7) << "export_dir_nicely " << *dir << " to " << dest << dendl;
  export_queue.push_back(pair<dirfrag_t,int>(dir->dirfrag(), dest));

  maybe_do_queued_export();
}

void Migrator::maybe_do_queued_export()
{
  while (!export_queue.empty() &&
	 export_state.size() <= 4) {
    dirfrag_t df = export_queue.front().first;
    int dest = export_queue.front().second;
    export_queue.pop_front();
    
    CDir *dir = mds->mdcache->get_dirfrag(df);
    if (!dir) continue;
    if (!dir->is_auth()) continue;

    dout(-7) << "nicely exporting to mds" << dest << " " << *dir << dendl;

    export_dir(dir, dest);
  }
}




class C_MDC_ExportFreeze : public Context {
  Migrator *mig;
  CDir *ex;   // dir i'm exporting

public:
  C_MDC_ExportFreeze(Migrator *m, CDir *e) :
	mig(m), ex(e) {}
  virtual void finish(int r) {
    if (r >= 0)
      mig->export_frozen(ex);
  }
};


/** export_dir(dir, dest)
 * public method to initiate an export.
 * will fail if the directory is freezing, frozen, unpinnable, or root. 
 */
void Migrator::export_dir(CDir *dir, int dest)
{
  dout(7) << "export_dir " << *dir << " to " << dest << dendl;
  assert(dir->is_auth());
  assert(dest != mds->get_nodeid());
   
  if (mds->mdsmap->is_degraded()) {
    dout(7) << "cluster degraded, no exports for now" << dendl;
    return;
  }

  if (dir->inode->is_root()) {
    dout(7) << "i won't export root" << dendl;
    //assert(0);
    return;
  }

  if (dir->is_frozen() ||
      dir->is_freezing()) {
    dout(7) << " can't export, freezing|frozen.  wait for other exports to finish first." << dendl;
    return;
  }
  if (dir->state_test(CDir::STATE_EXPORTING)) {
    dout(7) << "already exporting" << dendl;
    return;
  }
  
  // pin path?
  vector<CDentry*> trace;
  cache->make_trace(trace, dir->inode);
  if (!mds->locker->dentry_can_rdlock_trace(trace)) {
    dout(7) << "export_dir couldn't pin path, failing." << dendl;
    return;
  }

  // ok.
  mds->locker->dentry_anon_rdlock_trace_start(trace);
  assert(export_state.count(dir) == 0);
  export_state[dir] = EXPORT_DISCOVERING;
  export_peer[dir] = dest;

  dir->state_set(CDir::STATE_EXPORTING);

  // send ExportDirDiscover (ask target)
  mds->send_message_mds(new MExportDirDiscover(dir), dest);

  // start the freeze, but hold it up with an auth_pin.
  dir->auth_pin();
  dir->freeze_tree();
  assert(dir->is_freezing_tree());
  dir->add_waiter(CDir::WAIT_FROZEN, new C_MDC_ExportFreeze(this, dir));
}


/*
 * called on receipt of MExportDirDiscoverAck
 * the importer now has the directory's _inode_ in memory, and pinned.
 */
void Migrator::handle_export_discover_ack(MExportDirDiscoverAck *m)
{
  CDir *dir = cache->get_dirfrag(m->get_dirfrag());
  assert(dir);
  
  dout(7) << "export_discover_ack from " << m->get_source()
	  << " on " << *dir << dendl;

  if (export_state.count(dir) == 0 ||
      export_state[dir] != EXPORT_DISCOVERING ||
      export_peer[dir] != m->get_source().num()) {
    dout(7) << "must have aborted" << dendl;
  } else {
    // freeze the subtree
    export_state[dir] = EXPORT_FREEZING;
    dir->auth_unpin();
  }
  
  delete m;  // done
}

void Migrator::export_frozen(CDir *dir)
{
  dout(7) << "export_frozen on " << *dir << dendl;
  assert(dir->is_frozen());
  assert(dir->get_cum_auth_pins() == 0);

  // ok!
  int dest = export_peer[dir];

  cache->show_subtrees();

  // note the bounds.
  //  force it into a subtree by listing auth as <me,me>.
  cache->adjust_subtree_auth(dir, mds->get_nodeid(), mds->get_nodeid());
  set<CDir*> bounds;
  cache->get_subtree_bounds(dir, bounds);

  // generate prep message, log entry.
  MExportDirPrep *prep = new MExportDirPrep(dir->dirfrag());

  // include list of bystanders
  for (map<int,int>::iterator p = dir->replicas_begin();
       p != dir->replicas_end();
       p++) {
    if (p->first != dest) {
      dout(10) << "bystander mds" << p->first << dendl;
      prep->add_bystander(p->first);
    }
  }

  /* include spanning tree for all nested exports.
   * these need to be on the destination _before_ the final export so that
   * dir_auth updates on any nested exports are properly absorbed.
   * this includes inodes and dirfrags included in the subtree, but
   * only the inodes at the bounds.
   */
  set<inodeno_t> inodes_added;

  // include base dirfrag
  prep->add_dirfrag( new CDirDiscover(dir, dir->add_replica(dest)) );
  
  // check bounds
  for (set<CDir*>::iterator it = bounds.begin();
       it != bounds.end();
       it++) {
    CDir *bound = *it;

    // pin it.
    bound->get(CDir::PIN_EXPORTBOUND);
    bound->state_set(CDir::STATE_EXPORTBOUND);
    
    dout(7) << "  export bound " << *bound << dendl;

    prep->add_export( bound->dirfrag() );

    /* first assemble each trace, in trace order, and put in message */
    list<CInode*> inode_trace;  

    // trace to dir
    CDir *cur = bound;
    while (cur != dir) {
      // don't repeat ourselves
      if (inodes_added.count(cur->ino())) break;   // did already!
      inodes_added.insert(cur->ino());

      // inode
      assert(cur->inode->is_auth());
      inode_trace.push_front(cur->inode);
      dout(7) << "  will add " << *cur->inode << dendl;
      
      // include the dirfrag?  only if it's not the bounding subtree root.
      if (cur != bound) {
	assert(cur->is_auth());
        prep->add_dirfrag( cur->replicate_to(dest) );  // yay!
        dout(7) << "  added " << *cur << dendl;
      }
      
      cur = cur->get_parent_dir();
    }

    for (list<CInode*>::iterator it = inode_trace.begin();
         it != inode_trace.end();
         it++) {
      CInode *in = *it;
      dout(7) << "  added " << *in->parent << dendl;
      dout(7) << "  added " << *in << dendl;
      prep->add_inode( in->parent->get_dir()->dirfrag(),
		       in->parent->get_name(),
                       in->parent->replicate_to(dest),
                       in->replicate_to(dest) );
    }

  }

  // send.
  export_state[dir] = EXPORT_PREPPING;
  mds->send_message_mds(prep, dest);
}

void Migrator::handle_export_prep_ack(MExportDirPrepAck *m)
{
  CDir *dir = cache->get_dirfrag(m->get_dirfrag());
  assert(dir);

  dout(7) << "export_prep_ack " << *dir << dendl;

  if (export_state.count(dir) == 0 ||
      export_state[dir] != EXPORT_PREPPING) {
    // export must have aborted.  
    dout(7) << "export must have aborted" << dendl;
    delete m;
    return;
  }

  // send warnings
  int dest = export_peer[dir];
  set<CDir*> bounds;
  cache->get_subtree_bounds(dir, bounds);

  assert(export_peer.count(dir));
  assert(export_warning_ack_waiting.count(dir) == 0);
  assert(export_notify_ack_waiting.count(dir) == 0);

  for (map<int,int>::iterator p = dir->replicas_begin();
       p != dir->replicas_end();
       ++p) {
    if (p->first == dest) continue;
    if (!mds->mdsmap->is_active_or_stopping(p->first))
      continue;  // only if active
    export_warning_ack_waiting[dir].insert(p->first);
    export_notify_ack_waiting[dir].insert(p->first);  // we'll eventually get a notifyack, too!

    MExportDirNotify *notify = new MExportDirNotify(dir->dirfrag(), true,
						    pair<int,int>(mds->get_nodeid(),CDIR_AUTH_UNKNOWN),
						    pair<int,int>(mds->get_nodeid(),export_peer[dir]));
    notify->copy_bounds(bounds);
    mds->send_message_mds(notify, p->first);
    
  }
  export_state[dir] = EXPORT_WARNING;

  // nobody to warn?
  if (export_warning_ack_waiting.count(dir) == 0) 
    export_go(dir);  // start export.
    
  // done.
  delete m;
}


class C_M_ExportGo : public Context {
  Migrator *migrator;
  CDir *dir;
public:
  C_M_ExportGo(Migrator *m, CDir *d) : migrator(m), dir(d) {}
  void finish(int r) {
    migrator->export_go_synced(dir);
  }
};

void Migrator::export_go(CDir *dir)
{
  assert(export_peer.count(dir));
  int dest = export_peer[dir];
  dout(7) << "export_go " << *dir << " to " << dest << dendl;

  // first sync log to flush out e.g. any cap imports
  mds->mdlog->wait_for_sync(new C_M_ExportGo(this, dir));
}

void Migrator::export_go_synced(CDir *dir)
{  
  assert(export_peer.count(dir));
  int dest = export_peer[dir];
  dout(7) << "export_go_synced " << *dir << " to " << dest << dendl;

  cache->show_subtrees();
  
  export_warning_ack_waiting.erase(dir);
  export_state[dir] = EXPORT_EXPORTING;

  assert(dir->get_cum_auth_pins() == 0);

  // set ambiguous auth
  cache->adjust_subtree_auth(dir, dest, mds->get_nodeid());

  // take away the popularity we're sending.
  mds->balancer->subtract_export(dir);
  
  // fill export message with cache data
  utime_t now = g_clock.now();
  map<int,entity_inst_t> exported_client_map;
  bufferlist export_data;
  int num_exported_inodes = encode_export_dir( export_data,
					       dir,   // recur start point
					       exported_client_map,
					       now );
  bufferlist bl;
  ::_encode(exported_client_map, bl);
  bl.claim_append(export_data);
  export_data.claim(bl);

  // send the export data!
  MExportDir *req = new MExportDir(dir->dirfrag());
  req->take_dirstate(export_data);

  // add bounds to message
  set<CDir*> bounds;
  cache->get_subtree_bounds(dir, bounds);
  for (set<CDir*>::iterator p = bounds.begin();
       p != bounds.end();
       ++p)
    req->add_export((*p)->dirfrag());

  // send
  mds->send_message_mds(req, dest);

  // stats
  if (mds->logger) mds->logger->inc("ex");
  if (mds->logger) mds->logger->inc("iex", num_exported_inodes);

  cache->show_subtrees();
}


/** encode_export_inode
 * update our local state for this inode to export.
 * encode relevant state to be sent over the wire.
 * used by: encode_export_dir, file_rename (if foreign)
 *
 * FIXME: the separation between CInode.encode_export and these methods 
 * is pretty arbitrary and dumb.
 */
void Migrator::encode_export_inode(CInode *in, bufferlist& enc_state, 
				   map<int,entity_inst_t>& exported_client_map)
{
  dout(7) << "encode_export_inode " << *in << dendl;
  assert(!in->is_replica(mds->get_nodeid()));

  ::_encode_simple(in->inode.ino, enc_state);
  in->encode_export(enc_state);

  // caps 
  encode_export_inode_caps(in, enc_state, exported_client_map);
}

void Migrator::encode_export_inode_caps(CInode *in, bufferlist& bl, 
					map<int,entity_inst_t>& exported_client_map)
{
  // encode caps
  map<int,Capability::Export> cap_map;
  in->export_client_caps(cap_map);
  ::_encode_simple(cap_map, bl);

  in->state_set(CInode::STATE_EXPORTINGCAPS);

  // make note of clients named by exported capabilities
  for (map<int, Capability*>::iterator it = in->client_caps.begin();
       it != in->client_caps.end();
       it++) 
    exported_client_map[it->first] = mds->sessionmap.get_inst(entity_name_t::CLIENT(it->first));
}

void Migrator::finish_export_inode_caps(CInode *in)
{
  in->state_clear(CInode::STATE_EXPORTINGCAPS);

  // tell (all) clients about migrating caps.. 
  for (map<int, Capability*>::iterator it = in->client_caps.begin();
       it != in->client_caps.end();
       it++) {
    Capability *cap = it->second;
    dout(7) << "finish_export_inode telling client" << it->first
	    << " exported caps on " << *in << dendl;
    MClientFileCaps *m = new MClientFileCaps(CEPH_CAP_OP_EXPORT,
					     in->inode, 
                                             cap->get_last_seq(), 
                                             cap->pending(),
                                             cap->wanted());
    mds->send_message_client(m, it->first);
  }
  in->clear_client_caps();
}

void Migrator::finish_export_inode(CInode *in, utime_t now, list<Context*>& finished)
{
  dout(12) << "finish_export_inode " << *in << dendl;

  in->finish_export(now);

  finish_export_inode_caps(in);

  // relax locks?
  if (!in->is_replicated())
    in->replicate_relax_locks();

  // clean
  if (in->is_dirty()) in->mark_clean();
  
  // clear/unpin cached_by (we're no longer the authority)
  in->clear_replica_map();
  
  // twiddle lock states for auth -> replica transition
  in->authlock.export_twiddle();
  in->linklock.export_twiddle();
  in->dirfragtreelock.export_twiddle();
  in->filelock.export_twiddle();
  in->dirlock.export_twiddle();

  // mark auth
  assert(in->is_auth());
  in->state_clear(CInode::STATE_AUTH);
  in->replica_nonce = CInode::EXPORT_NONCE;
  
  // waiters
  in->take_waiting(CInode::WAIT_ANY, finished);
  
  // *** other state too?

  // move to end of LRU so we drop out of cache quickly!
  if (in->get_parent_dn()) 
    cache->lru.lru_bottouch(in->get_parent_dn());

}

int Migrator::encode_export_dir(bufferlist& exportbl,
				CDir *dir,
				map<int,entity_inst_t>& exported_client_map,
				utime_t now)
{
  int num_exported = 0;

  dout(7) << "encode_export_dir " << *dir << " " << dir->nitems << " items" << dendl;
  
  assert(dir->get_projected_version() == dir->get_version());

  // dir 
  dirfrag_t df = dir->dirfrag();
  ::_encode_simple(df, exportbl);
  dir->encode_export(exportbl);
  
  long nden = dir->items.size();
  ::_encode_simple(nden, exportbl);
  
  // dentries
  list<CDir*> subdirs;
  CDir::map_t::iterator it;
  for (it = dir->begin(); it != dir->end(); it++) {
    CDentry *dn = it->second;
    CInode *in = dn->get_inode();
    
    num_exported++;
    
    // -- dentry
    dout(7) << "encode_export_dir exporting " << *dn << dendl;
    
    // dn name
    ::_encode(it->first, exportbl);
    
    // state
    dn->encode_export(exportbl);
    
    // points to...
    
    // null dentry?
    if (dn->is_null()) {
      exportbl.append("N", 1);  // null dentry
      continue;
    }
    
    if (dn->is_remote()) {
      // remote link
      exportbl.append("L", 1);  // remote link
      
      inodeno_t ino = dn->get_remote_ino();
      unsigned char d_type = dn->get_remote_d_type();
      ::_encode(ino, exportbl);
      ::_encode(d_type, exportbl);
      continue;
    }
    
    // primary link
    // -- inode
    exportbl.append("I", 1);    // inode dentry
    
    encode_export_inode(in, exportbl, exported_client_map);  // encode, and (update state for) export
    
    // directory?
    list<CDir*> dfs;
    in->get_dirfrags(dfs);
    for (list<CDir*>::iterator p = dfs.begin(); p != dfs.end(); ++p) {
      CDir *dir = *p;
      if (!dir->state_test(CDir::STATE_EXPORTBOUND)) {
	// include nested dirfrag
	assert(dir->get_dir_auth().first == CDIR_AUTH_PARENT);
	subdirs.push_back(dir);  // it's ours, recurse (later)
      }
    }
  }

  // subdirs
  for (list<CDir*>::iterator it = subdirs.begin(); it != subdirs.end(); it++)
    num_exported += encode_export_dir(exportbl, *it, exported_client_map, now);

  return num_exported;
}

void Migrator::finish_export_dir(CDir *dir, list<Context*>& finished, utime_t now)
{
  dout(10) << "finish_export_dir " << *dir << dendl;

  // release open_by 
  dir->clear_replica_map();

  // mark
  assert(dir->is_auth());
  dir->state_clear(CDir::STATE_AUTH);
  dir->replica_nonce = CDir::NONCE_EXPORT;

  if (dir->is_dirty())
    dir->mark_clean();
  
  // discard most dir state
  dir->state &= CDir::MASK_STATE_EXPORT_KEPT;  // i only retain a few things.

  // suck up all waiters
  dir->take_waiting(CDir::WAIT_ANY, finished);    // all dir waiters
  
  // pop
  dir->finish_export(now);

  // dentries
  list<CDir*> subdirs;
  CDir::map_t::iterator it;
  for (it = dir->begin(); it != dir->end(); it++) {
    CDentry *dn = it->second;
    CInode *in = dn->get_inode();

    // dentry
    dn->finish_export();

    // inode?
    if (dn->is_primary()) {
      finish_export_inode(in, now, finished);

      // subdirs?
      in->get_nested_dirfrags(subdirs);
    }
  }

  // subdirs
  for (list<CDir*>::iterator it = subdirs.begin(); it != subdirs.end(); it++) 
    finish_export_dir(*it, finished, now);
}

class C_MDS_ExportFinishLogged : public Context {
  Migrator *migrator;
  CDir *dir;
public:
  C_MDS_ExportFinishLogged(Migrator *m, CDir *d) : migrator(m), dir(d) {}
  void finish(int r) {
    migrator->export_logged_finish(dir);
  }
};


/*
 * i should get an export_ack from the export target.
 */
void Migrator::handle_export_ack(MExportDirAck *m)
{
  CDir *dir = cache->get_dirfrag(m->get_dirfrag());
  assert(dir);
  assert(dir->is_frozen_tree_root());  // i'm exporting!

  // yay!
  dout(7) << "handle_export_ack " << *dir << dendl;

  export_warning_ack_waiting.erase(dir);
  
  export_state[dir] = EXPORT_LOGGINGFINISH;
  
  set<CDir*> bounds;
  cache->get_subtree_bounds(dir, bounds);

  // log completion. 
  //  include export bounds, to ensure they're in the journal.
  EExport *le = new EExport(mds->mdlog, dir);
  le->metablob.add_dir_context(dir);
  le->metablob.add_dir( dir, false );
  for (set<CDir*>::iterator p = bounds.begin();
       p != bounds.end();
       ++p) {
    CDir *bound = *p;
    le->get_bounds().insert(bound->dirfrag());
    le->metablob.add_dir_context(bound);
    le->metablob.add_dir(bound, false);
  }

  // log export completion, then finish (unfreeze, trigger finish context, etc.)
  mds->mdlog->submit_entry(le,
			   new C_MDS_ExportFinishLogged(this, dir));
  
  delete m;
}





/*
 * this happens if hte dest failes after i send teh export data but before it is acked
 * that is, we don't know they safely received and logged it, so we reverse our changes
 * and go on.
 */
void Migrator::export_reverse(CDir *dir)
{
  dout(7) << "export_reverse " << *dir << dendl;
  
  assert(export_state[dir] == EXPORT_EXPORTING);
  
  set<CDir*> bounds;
  cache->get_subtree_bounds(dir, bounds);

  // adjust auth, with possible subtree merge.
  cache->adjust_subtree_auth(dir, mds->get_nodeid());
  cache->try_subtree_merge(dir);

  // remove exporting pins
  list<CDir*> rq;
  rq.push_back(dir);
  while (!rq.empty()) {
    CDir *dir = rq.front(); 
    rq.pop_front();
    dir->abort_export();
    for (CDir::map_t::iterator p = dir->items.begin(); p != dir->items.end(); ++p) {
      p->second->abort_export();
      if (!p->second->is_primary()) continue;
      CInode *in = p->second->get_inode();
      in->abort_export();
      if (in->is_dir())
	in->get_nested_dirfrags(rq);
    }
  }
  
  // unpin bounds
  for (set<CDir*>::iterator p = bounds.begin();
       p != bounds.end();
       ++p) {
    CDir *bd = *p;
    bd->put(CDir::PIN_EXPORTBOUND);
    bd->state_clear(CDir::STATE_EXPORTBOUND);
  }

  // process delayed expires
  cache->process_delayed_expire(dir);
  
  // some clean up
  export_warning_ack_waiting.erase(dir);
  export_notify_ack_waiting.erase(dir);

  // unfreeze
  dir->unfreeze_tree();

  cache->show_cache();
}


/*
 * once i get the ack, and logged the EExportFinish(true),
 * send notifies (if any), otherwise go straight to finish.
 * 
 */
void Migrator::export_logged_finish(CDir *dir)
{
  dout(7) << "export_logged_finish " << *dir << dendl;

  // send notifies
  int dest = export_peer[dir];

  set<CDir*> bounds;
  cache->get_subtree_bounds(dir, bounds);

  for (set<int>::iterator p = export_notify_ack_waiting[dir].begin();
       p != export_notify_ack_waiting[dir].end();
       ++p) {
    MExportDirNotify *notify;
    if (mds->mdsmap->is_active_or_stopping(export_peer[dir])) 
      // dest is still alive.
      notify = new MExportDirNotify(dir->dirfrag(), true,
				    pair<int,int>(mds->get_nodeid(), dest),
				    pair<int,int>(dest, CDIR_AUTH_UNKNOWN));
    else 
      // dest is dead.  bystanders will think i am only auth, as per mdcache->handle_mds_failure()
      notify = new MExportDirNotify(dir->dirfrag(), true,
				    pair<int,int>(mds->get_nodeid(), CDIR_AUTH_UNKNOWN),
				    pair<int,int>(dest, CDIR_AUTH_UNKNOWN));

    notify->copy_bounds(bounds);
    
    mds->send_message_mds(notify, *p);
  }

  // wait for notifyacks
  export_state[dir] = EXPORT_NOTIFYING;
  
  // no notifies to wait for?
  if (export_notify_ack_waiting[dir].empty())
    export_finish(dir);  // skip notify/notify_ack stage.
}

/*
 * warning:
 *  i'll get an ack from each bystander.
 *  when i get them all, do the export.
 * notify:
 *  i'll get an ack from each bystander.
 *  when i get them all, unfreeze and send the finish.
 */
void Migrator::handle_export_notify_ack(MExportDirNotifyAck *m)
{
  CDir *dir = cache->get_dirfrag(m->get_dirfrag());
  assert(dir);
  int from = m->get_source().num();
    
  if (export_state.count(dir) && export_state[dir] == EXPORT_WARNING) {
    // exporting. process warning.
    dout(7) << "handle_export_notify_ack from " << m->get_source()
	    << ": exporting, processing warning on "
	    << *dir << dendl;
    assert(export_warning_ack_waiting.count(dir));
    export_warning_ack_waiting[dir].erase(from);
    
    if (export_warning_ack_waiting[dir].empty()) 
      export_go(dir);     // start export.
  } 
  else if (export_state.count(dir) && export_state[dir] == EXPORT_NOTIFYING) {
    // exporting. process notify.
    dout(7) << "handle_export_notify_ack from " << m->get_source()
	    << ": exporting, processing notify on "
	    << *dir << dendl;
    assert(export_notify_ack_waiting.count(dir));
    export_notify_ack_waiting[dir].erase(from);
    
    if (export_notify_ack_waiting[dir].empty())
      export_finish(dir);
  }
  else if (import_state.count(dir->dirfrag()) && import_state[dir->dirfrag()] == IMPORT_ABORTING) {
    // reversing import
    dout(7) << "handle_export_notify_ack from " << m->get_source()
	    << ": aborting import on "
	    << *dir << dendl;
    assert(import_bystanders[dir].count(from));
    import_bystanders[dir].erase(from);
    if (import_bystanders[dir].empty()) {
      import_bystanders.erase(dir);
      import_reverse_unfreeze(dir);
    }
  }

  delete m;
}


void Migrator::export_finish(CDir *dir)
{
  dout(5) << "export_finish " << *dir << dendl;

  if (export_state.count(dir) == 0) {
    dout(7) << "target must have failed, not sending final commit message.  export succeeded anyway." << dendl;
    return;
  }

  // send finish/commit to new auth
  if (mds->mdsmap->is_active_or_stopping(export_peer[dir])) {
    mds->send_message_mds(new MExportDirFinish(dir->dirfrag()), export_peer[dir]);
  } else {
    dout(7) << "not sending MExportDirFinish, dest has failed" << dendl;
  }
  
  // finish export (adjust local cache state)
  C_Contexts *fin = new C_Contexts;
  finish_export_dir(dir, fin->contexts, g_clock.now());
  dir->add_waiter(CDir::WAIT_UNFREEZE, fin);

  // unfreeze
  dout(7) << "export_finish unfreezing" << dendl;
  dir->unfreeze_tree();
  
  // unpin bounds
  set<CDir*> bounds;
  cache->get_subtree_bounds(dir, bounds);
  for (set<CDir*>::iterator p = bounds.begin();
       p != bounds.end();
       ++p) {
    CDir *bd = *p;
    bd->put(CDir::PIN_EXPORTBOUND);
    bd->state_clear(CDir::STATE_EXPORTBOUND);
  }

  // adjust auth, with possible subtree merge.
  //  (we do this _after_ removing EXPORTBOUND pins, to allow merges)
  cache->adjust_subtree_auth(dir, export_peer[dir]);
  cache->try_subtree_merge(dir);
  
  // unpin path
  dout(7) << "export_finish unpinning path" << dendl;
  vector<CDentry*> trace;
  cache->make_trace(trace, dir->inode);
  mds->locker->dentry_anon_rdlock_trace_finish(trace);

  // discard delayed expires
  cache->discard_delayed_expire(dir);

  // remove from exporting list, clean up state
  dir->state_clear(CDir::STATE_EXPORTING);
  export_state.erase(dir);
  export_peer.erase(dir);
  export_notify_ack_waiting.erase(dir);

  // queue finishers
  mds->queue_waiters(export_finish_waiters[dir]);
  export_finish_waiters.erase(dir);

  cache->show_subtrees();
  audit();

  // send pending import_maps?
  mds->mdcache->maybe_send_pending_resolves();
  
  maybe_do_queued_export();
}








// ==========================================================
// IMPORT

void Migrator::handle_export_discover(MExportDirDiscover *m)
{
  assert(m->get_source().num() != mds->get_nodeid());

  dout(7) << "handle_export_discover on " << m->get_path() << dendl;

  // note import state
  dirfrag_t df = m->get_dirfrag();
  
  // only start discovering on this message once.
  if (!m->started) {
    m->started = true;
    import_state[df] = IMPORT_DISCOVERING;
    import_peer[df] = m->get_source().num();
  }

  // am i retrying after ancient path_traverse results?
  if (import_state.count(df) == 0 &&
      import_state[df] != IMPORT_DISCOVERING) {
    dout(7) << "hmm import_state is off, i must be obsolete lookup" << dendl;
    delete m;
    return;
  }

  // do we have it?
  CInode *in = cache->get_inode(m->get_dirfrag().ino);
  if (!in) {
    // must discover it!
    filepath fpath(m->get_path());
    vector<CDentry*> trace;
    int r = cache->path_traverse(0, m, fpath, trace, true, MDS_TRAVERSE_DISCOVER);
    if (r > 0) return; // wait
    if (r < 0) {
      dout(7) << "handle_export_discover_2 failed to discover or not dir " << m->get_path() << ", NAK" << dendl;
      assert(0);    // this shouldn't happen if the auth pins his path properly!!!! 
    }

    assert(0); // this shouldn't happen; the get_inode above would have succeeded.
  }

  // yay
  dout(7) << "handle_export_discover have " << df << " inode " << *in << dendl;
  
  import_state[m->get_dirfrag()] = IMPORT_DISCOVERED;
  
  // pin inode in the cache (for now)
  assert(in->is_dir());
  in->get(CInode::PIN_IMPORTING);

  // reply
  dout(7) << " sending export_discover_ack on " << *in << dendl;
  mds->send_message_mds(new MExportDirDiscoverAck(df), import_peer[df]);
}

void Migrator::handle_export_cancel(MExportDirCancel *m)
{
  dout(7) << "handle_export_cancel on " << m->get_dirfrag() << dendl;

  if (import_state[m->get_dirfrag()] == IMPORT_DISCOVERED) {
    CInode *in = cache->get_inode(m->get_dirfrag().ino);
    assert(in);
    in->put(CInode::PIN_IMPORTING);
  } else {
    assert(import_state[m->get_dirfrag()] == IMPORT_DISCOVERING);
  }

  import_state.erase(m->get_dirfrag());
  import_peer.erase(m->get_dirfrag());

  delete m;
}


void Migrator::handle_export_prep(MExportDirPrep *m)
{
  int oldauth = m->get_source().num();
  assert(oldauth != mds->get_nodeid());

  // make sure we didn't abort
  if (import_state.count(m->get_dirfrag()) == 0 ||
      (import_state[m->get_dirfrag()] != IMPORT_DISCOVERED &&
       import_state[m->get_dirfrag()] != IMPORT_PREPPING) ||
      import_peer[m->get_dirfrag()] != oldauth) {
    dout(10) << "handle_export_prep import has aborted, dropping" << dendl;
    delete m;
    return;
  }

  CInode *diri = cache->get_inode(m->get_dirfrag().ino);
  assert(diri);
  
  list<Context*> finished;

  // assimilate root dir.
  CDir *dir;

  if (!m->did_assim()) {
    dir = cache->add_replica_dir(diri, 
				 m->get_dirfrag().frag, *m->get_dirfrag_discover(m->get_dirfrag()), 
				 oldauth, finished);
    dout(7) << "handle_export_prep on " << *dir << " (first pass)" << dendl;
  } else {
    dir = cache->get_dirfrag(m->get_dirfrag());
    assert(dir);
    dout(7) << "handle_export_prep on " << *dir << " (subsequent pass)" << dendl;
  }
  assert(dir->is_auth() == false);

  cache->show_subtrees();

  // build import bound map
  map<inodeno_t, fragset_t> import_bound_fragset;
  for (list<dirfrag_t>::iterator p = m->get_bounds().begin();
       p != m->get_bounds().end();
       ++p) {
    dout(10) << " bound " << *p << dendl;
    import_bound_fragset[p->ino].insert(p->frag);
  }

  // assimilate contents?
  if (!m->did_assim()) {
    dout(7) << "doing assim on " << *dir << dendl;
    m->mark_assim();  // only do this the first time!

    // move pin to dir
    diri->put(CInode::PIN_IMPORTING);
    dir->get(CDir::PIN_IMPORTING);  
    dir->state_set(CDir::STATE_IMPORTING);

    // change import state
    import_state[dir->dirfrag()] = IMPORT_PREPPING;
    import_bound_ls[dir] = m->get_bounds();
    
    // bystander list
    import_bystanders[dir] = m->get_bystanders();
    dout(7) << "bystanders are " << import_bystanders[dir] << dendl;

    // assimilate traces to exports
    for (list<CInodeDiscover*>::iterator it = m->get_inodes().begin();
         it != m->get_inodes().end();
         it++) {
      // inode
      CInode *in = cache->get_inode( (*it)->get_ino() );
      if (in) {
        (*it)->update_inode(in);
        dout(7) << " updated " << *in << dendl;
      } else {
        in = new CInode(mds->mdcache, false);
        (*it)->update_inode(in);
        
        // link to the containing dir
	CDir *condir = cache->get_dirfrag( m->get_containing_dirfrag(in->ino()) );
	assert(condir);
	cache->add_inode( in );
        condir->add_primary_dentry( m->get_dentry(in->ino()), in );
        
        dout(7) << "   added " << *in << dendl;
      }
      
      assert( in->get_parent_dir()->dirfrag() == m->get_containing_dirfrag(in->ino()) );
      
      // dirs
      for (list<frag_t>::iterator pf = m->get_inode_dirfrags(in->ino()).begin();
	   pf != m->get_inode_dirfrags(in->ino()).end();
	   ++pf) {
	// add/update
	cache->add_replica_dir(in, *pf, *m->get_dirfrag_discover(dirfrag_t(in->ino(), *pf)),
			       oldauth, finished);
      }
    }

    // make bound sticky
    for (map<inodeno_t,fragset_t>::iterator p = import_bound_fragset.begin();
	 p != import_bound_fragset.end();
	 ++p) {
      CInode *in = cache->get_inode(p->first);
      assert(in);
      in->get_stickydirs();
      dout(7) << " set stickydirs on bound inode " << *in << dendl;
    }

  } else {
    dout(7) << " not doing assim on " << *dir << dendl;
  }

  if (!finished.empty())
    mds->queue_waiters(finished);


  // open all bounds
  set<CDir*> import_bounds;
  for (map<inodeno_t,fragset_t>::iterator p = import_bound_fragset.begin();
       p != import_bound_fragset.end();
       ++p) {
    CInode *in = cache->get_inode(p->first);
    assert(in);

    // map fragset into a frag_t list, based on the inode fragtree
    list<frag_t> fglist;
    for (set<frag_t>::iterator q = p->second.begin(); q != p->second.end(); ++q)
      in->dirfragtree.get_leaves_under(*q, fglist);
    dout(10) << " bound inode " << p->first << " fragset " << p->second << " maps to " << fglist << dendl;
    
    for (list<frag_t>::iterator q = fglist.begin();
	 q != fglist.end();
	 ++q) {
      CDir *bound = cache->get_dirfrag(dirfrag_t(p->first, *q));
      if (!bound) {
	dout(7) << "  opening bounding dirfrag " << *q << " on " << *in << dendl;
	cache->open_remote_dirfrag(in, *q,
				   new C_MDS_RetryMessage(mds, m));
	return;
      }

      if (!bound->state_test(CDir::STATE_IMPORTBOUND)) {
	dout(7) << "  pinning import bound " << *bound << dendl;
	bound->get(CDir::PIN_IMPORTBOUND);
	bound->state_set(CDir::STATE_IMPORTBOUND);
      } else {
	dout(7) << "  already pinned import bound " << *bound << dendl;
      }
      import_bounds.insert(bound);
    }
  }

  dout(7) << " all ready, noting auth and freezing import region" << dendl;
  
  // note that i am an ambiguous auth for this subtree.
  // specify bounds, since the exporter explicitly defines the region.
  cache->adjust_bounded_subtree_auth(dir, import_bounds, 
				     pair<int,int>(oldauth, mds->get_nodeid()));
  cache->verify_subtree_bounds(dir, import_bounds);
  
  // freeze.
  dir->_freeze_tree();
  
  // ok!
  dout(7) << " sending export_prep_ack on " << *dir << dendl;
  mds->send_message_mds(new MExportDirPrepAck(dir->dirfrag()), m->get_source().num());
  
  // note new state
  import_state[dir->dirfrag()] = IMPORT_PREPPED;
  
  // done 
  delete m;

}




class C_MDS_ImportDirLoggedStart : public Context {
  Migrator *migrator;
  CDir *dir;
  int from;
public:
  map<int,entity_inst_t> imported_client_map;

  C_MDS_ImportDirLoggedStart(Migrator *m, CDir *d, int f) :
    migrator(m), dir(d), from(f) {
  }
  void finish(int r) {
    migrator->import_logged_start(dir, from, imported_client_map);
  }
};

void Migrator::handle_export_dir(MExportDir *m)
{
  CDir *dir = cache->get_dirfrag(m->get_dirfrag());
  assert(dir);

  int oldauth = m->get_source().num();
  dout(7) << "handle_export_dir importing " << *dir << " from " << oldauth << dendl;
  assert(dir->is_auth() == false);

  cache->show_subtrees();

  C_MDS_ImportDirLoggedStart *onlogged = new C_MDS_ImportDirLoggedStart(this, dir, m->get_source().num());

  // start the journal entry
  EImportStart *le = new EImportStart(dir->dirfrag(), m->get_bounds());
  le->metablob.add_dir_context(dir);
  
  // adjust auth (list us _first_)
  cache->adjust_subtree_auth(dir, mds->get_nodeid(), oldauth);

  // add this crap to my cache
  bufferlist::iterator blp = m->get_dirstate().begin();

  // new client sessions, open these after we journal
  ::_decode_simple(onlogged->imported_client_map, blp);
  mds->server->prepare_force_open_sessions(onlogged->imported_client_map);

  int num_imported_inodes = 0;
  while (!blp.end()) {
    num_imported_inodes += 
      decode_import_dir(blp,
			oldauth, 
			dir,                 // import root
			le,
			mds->mdlog->get_current_segment(),
			import_caps[dir],
			import_updated_scatterlocks[dir]);
  }
  dout(10) << " " << m->get_bounds().size() << " imported bounds" << dendl;
  
  // include imported sessions in EImportStart
  le->client_map.claim(m->get_dirstate());

  // include bounds in EImportStart
  set<CDir*> import_bounds;
  cache->get_subtree_bounds(dir, import_bounds);
  for (set<CDir*>::iterator it = import_bounds.begin();
       it != import_bounds.end();
       it++) 
    le->metablob.add_dir(*it, false);  // note that parent metadata is already in the event

  // adjust popularity
  mds->balancer->add_import(dir);

  dout(7) << "handle_export_dir did " << *dir << dendl;

  // log it
  mds->mdlog->submit_entry(le, onlogged);

  // note state
  import_state[dir->dirfrag()] = IMPORT_LOGGINGSTART;

  // some stats
  if (mds->logger) {
    mds->logger->inc("im");
    mds->logger->inc("iim", num_imported_inodes);
  }

  delete m;
}


/*
 * this is an import helper
 *  called by import_finish, and import_reverse and friends.
 */
void Migrator::import_remove_pins(CDir *dir, set<CDir*>& bounds)
{
  // root
  dir->put(CDir::PIN_IMPORTING);
  dir->state_clear(CDir::STATE_IMPORTING);

  // bounds
  set<CInode*> didinodes;
  for (set<CDir*>::iterator it = bounds.begin();
       it != bounds.end();
       it++) {
    CDir *bd = *it;
    bd->put(CDir::PIN_IMPORTBOUND);
    bd->state_clear(CDir::STATE_IMPORTBOUND);
    CInode *bdi = bd->get_inode();
    if (didinodes.count(bdi) == 0) {
      bdi->put_stickydirs();
      didinodes.insert(bdi);
    }
  }
}


/*
 * note: this does teh full work of reversing and import and cleaning up
 *  state.  
 * called by both handle_mds_failure and by handle_resolve (if we are
 *  a survivor coping with an exporter failure+recovery).
 */
void Migrator::import_reverse(CDir *dir)
{
  dout(7) << "import_reverse " << *dir << dendl;

  set<CDir*> bounds;
  cache->get_subtree_bounds(dir, bounds);

  // remove pins
  import_remove_pins(dir, bounds);

  // update auth, with possible subtree merge.
  assert(dir->is_subtree_root());
  cache->adjust_subtree_auth(dir, import_peer[dir->dirfrag()]);
  cache->try_subtree_merge(dir);

  // adjust auth bits.
  list<CDir*> q;
  q.push_back(dir);
  while (!q.empty()) {
    CDir *cur = q.front();
    q.pop_front();
    
    // dir
    assert(cur->is_auth());
    cur->state_clear(CDir::STATE_AUTH);
    cur->clear_replica_map();
    if (cur->is_dirty())
      cur->mark_clean();

    CDir::map_t::iterator it;
    for (it = cur->begin(); it != cur->end(); it++) {
      CDentry *dn = it->second;

      // dentry
      dn->state_clear(CDentry::STATE_AUTH);
      dn->clear_replica_map();
      if (dn->is_dirty()) 
	dn->mark_clean();

      // inode?
      if (dn->is_primary()) {
	CInode *in = dn->get_inode();
	in->state_clear(CDentry::STATE_AUTH);
	in->clear_replica_map();
	if (in->is_dirty()) 
	  in->mark_clean();
	in->authlock.clear_gather();
	in->linklock.clear_gather();
	in->dirfragtreelock.clear_gather();
	in->filelock.clear_gather();

	// non-bounding dir?
	list<CDir*> dfs;
	in->get_dirfrags(dfs);
	for (list<CDir*>::iterator p = dfs.begin(); p != dfs.end(); ++p)
	  if (bounds.count(*p) == 0)
	    q.push_back(*p);
      }
    }
  }

  // reexport caps
  for (map<CInode*, map<int,Capability::Export> >::iterator p = import_caps[dir].begin();
       p != import_caps[dir].end();
       ++p) {
    CInode *in = p->first;
    /*
     * bleh.. just export all caps for this inode.  the auth mds
     * will pick them up during recovery.
     */
    map<int,Capability::Export> cap_map;  // throw this away
    in->export_client_caps(cap_map);
    finish_export_inode_caps(in);
  }
	 
  // log our failure
  mds->mdlog->submit_entry(new EImportFinish(dir, false));	// log failure
       
  // bystanders?
  if (import_bystanders[dir].empty()) {
    dout(7) << "no bystanders, finishing reverse now" << dendl;
    import_reverse_unfreeze(dir);
  } else {
    // notify them; wait in aborting state
    dout(7) << "notifying bystanders of abort" << dendl;
    import_notify_abort(dir, bounds);
    import_state[dir->dirfrag()] = IMPORT_ABORTING;
  }
}

void Migrator::import_notify_abort(CDir *dir, set<CDir*>& bounds)
{
  dout(7) << "import_notify_abort " << *dir << dendl;
  
  for (set<int>::iterator p = import_bystanders[dir].begin();
       p != import_bystanders[dir].end();
       ++p) {
    // NOTE: the bystander will think i am _only_ auth, because they will have seen
    // the exporter's failure and updated the subtree auth.  see mdcache->handle_mds_failure().
    MExportDirNotify *notify = 
      new MExportDirNotify(dir->dirfrag(), true,
			   pair<int,int>(mds->get_nodeid(), CDIR_AUTH_UNKNOWN),
			   pair<int,int>(import_peer[dir->dirfrag()], CDIR_AUTH_UNKNOWN));
    notify->copy_bounds(bounds);
    mds->send_message_mds(notify, *p);
  }
}

void Migrator::import_reverse_unfreeze(CDir *dir)
{
  dout(7) << "import_reverse_unfreeze " << *dir << dendl;
  dir->unfreeze_tree();
  cache->discard_delayed_expire(dir);
  import_reverse_final(dir);
}

void Migrator::import_reverse_final(CDir *dir) 
{
  dout(7) << "import_reverse_final " << *dir << dendl;

  // clean up
  import_state.erase(dir->dirfrag());
  import_peer.erase(dir->dirfrag());
  import_bystanders.erase(dir);
  import_bound_ls.erase(dir);
  import_updated_scatterlocks.erase(dir);
  import_caps.erase(dir);

  // send pending import_maps?
  mds->mdcache->maybe_send_pending_resolves();

  cache->show_subtrees();
  //audit();  // this fails, bc we munge up the subtree map during handle_import_map (resolve phase)
}




void Migrator::import_logged_start(CDir *dir, int from,
				   map<int,entity_inst_t>& imported_client_map)
{
  dout(7) << "import_logged " << *dir << dendl;

  // note state
  import_state[dir->dirfrag()] = IMPORT_ACKING;

  // force open client sessions and finish cap import
  mds->server->finish_force_open_sessions(imported_client_map);
  
  for (map<CInode*, map<int,Capability::Export> >::iterator p = import_caps[dir].begin();
       p != import_caps[dir].end();
       ++p) {
    finish_import_inode_caps(p->first, from, p->second);
  }
  
  // send notify's etc.
  dout(7) << "sending ack for " << *dir << " to old auth mds" << from << dendl;
  mds->send_message_mds(new MExportDirAck(dir->dirfrag()), from);

  cache->show_subtrees();
}


void Migrator::handle_export_finish(MExportDirFinish *m)
{
  CDir *dir = cache->get_dirfrag(m->get_dirfrag());
  assert(dir);
  dout(7) << "handle_export_finish on " << *dir << dendl;
  import_finish(dir);
  delete m;
}

void Migrator::import_finish(CDir *dir) 
{
  dout(7) << "import_finish on " << *dir << dendl;

  // log finish
  mds->mdlog->submit_entry(new EImportFinish(dir, true));

  // clear updated scatterlocks
  for (list<ScatterLock*>::iterator p = import_updated_scatterlocks[dir].begin();
       p != import_updated_scatterlocks[dir].end();
       ++p) 
    (*p)->clear_updated();

  // remove pins
  set<CDir*> bounds;
  cache->get_subtree_bounds(dir, bounds);
  import_remove_pins(dir, bounds);

  // adjust auth, with possible subtree merge.
  cache->adjust_subtree_auth(dir, mds->get_nodeid());
  cache->try_subtree_merge(dir);
  
  // clear import state (we're done!)
  import_state.erase(dir->dirfrag());
  import_peer.erase(dir->dirfrag());
  import_bystanders.erase(dir);
  import_bound_ls.erase(dir);
  import_caps.erase(dir);
  import_updated_scatterlocks.erase(dir);

  // process delayed expires
  cache->process_delayed_expire(dir);

  // ok now unfreeze (and thus kick waiters)
  dir->unfreeze_tree();

  cache->show_subtrees();
  //audit();  // this fails, bc we munge up the subtree map during handle_import_map (resolve phase)

  // send pending import_maps?
  mds->mdcache->maybe_send_pending_resolves();

  // is it empty?
  if (dir->get_size() == 0 &&
      !dir->inode->is_auth()) {
    // reexport!
    export_empty_import(dir);
  }
}


void Migrator::decode_import_inode(CDentry *dn, bufferlist::iterator& blp, int oldauth,
				   LogSegment *ls,
				   map<CInode*, map<int,Capability::Export> >& cap_imports,
				   list<ScatterLock*>& updated_scatterlocks)
{  
  dout(15) << "decode_import_inode on " << *dn << dendl;

  inodeno_t ino;
  ::_decode_simple(ino, blp);
  
  bool added = false;
  CInode *in = cache->get_inode(ino);
  if (!in) {
    in = new CInode(mds->mdcache);
    added = true;
  } else {
    in->state_set(CInode::STATE_AUTH);
  }

  // state after link  -- or not!  -sage
  in->decode_import(blp, ls);  // cap imports are noted for later action

  // caps
  decode_import_inode_caps(in, blp, cap_imports);

  // link before state  -- or not!  -sage
  if (dn->inode != in) {
    assert(!dn->inode);
    dn->dir->link_primary_inode(dn, in);
  }
 
  // add inode?
  if (added) {
    cache->add_inode(in);
    dout(10) << "added " << *in << dendl;
  } else {
    dout(10) << "  had " << *in << dendl;
  }
  
  // clear if dirtyscattered, since we're going to journal this
  //  but not until we _actually_ finish the import...
  if (in->dirlock.is_updated())
    updated_scatterlocks.push_back(&in->dirlock);

  // put in autoscatter list?
  //  this is conservative, but safe.
  if (in->dirlock.get_state() == LOCK_SCATTER)
    mds->locker->note_autoscattered(&in->dirlock);
  
  // adjust replica list
  //assert(!in->is_replica(oldauth));  // not true on failed export
  in->add_replica(oldauth, CInode::EXPORT_NONCE);
  if (in->is_replica(mds->get_nodeid()))
    in->remove_replica(mds->get_nodeid());
  
}

void Migrator::decode_import_inode_caps(CInode *in,
					bufferlist::iterator &blp,
					map<CInode*, map<int,Capability::Export> >& cap_imports)
{
  map<int,Capability::Export> cap_map;
  ::_decode_simple(cap_map, blp);
  if (!cap_map.empty()) {
    cap_imports[in].swap(cap_map);
    in->get(CInode::PIN_IMPORTINGCAPS);
  }
}

void Migrator::finish_import_inode_caps(CInode *in, int from, 
					map<int,Capability::Export> &cap_map)
{
  assert(!cap_map.empty());
  
  for (map<int,Capability::Export>::iterator it = cap_map.begin();
       it != cap_map.end();
       it++) {
    dout(0) << "finish_import_inode_caps for client" << it->first << " on " << *in << dendl;
    Session *session = mds->sessionmap.get_session(entity_name_t::CLIENT(it->first));
    assert(session);

    Capability *cap = in->get_client_cap(it->first);
    if (!cap) 
      cap = in->add_client_cap(it->first, in, session->caps);
    cap->merge(it->second);

    MClientFileCaps *caps = new MClientFileCaps(CEPH_CAP_OP_IMPORT,
						in->inode,
						cap->get_last_seq(),
						cap->pending(),
						cap->wanted(),
						from);
    mds->send_message_client(caps, session->inst);
  }

  in->put(CInode::PIN_IMPORTINGCAPS);
}

int Migrator::decode_import_dir(bufferlist::iterator& blp,
				int oldauth,
				CDir *import_root,
				EImportStart *le,
				LogSegment *ls,
				map<CInode*, map<int,Capability::Export> >& cap_imports,
				list<ScatterLock*>& updated_scatterlocks)
{
  // set up dir
  dirfrag_t df;
  ::_decode_simple(df, blp);

  CInode *diri = cache->get_inode(df.ino);
  assert(diri);
  CDir *dir = diri->get_or_open_dirfrag(mds->mdcache, df.frag);
  assert(dir);
  
  dout(7) << "decode_import_dir " << *dir << dendl;

  // assimilate state
  dir->decode_import(blp);

  // mark  (may already be marked from get_or_open_dir() above)
  if (!dir->is_auth())
    dir->state_set(CDir::STATE_AUTH);

  // adjust replica list
  //assert(!dir->is_replica(oldauth));    // not true on failed export
  dir->add_replica(oldauth);
  if (dir->is_replica(mds->get_nodeid()))
    dir->remove_replica(mds->get_nodeid());

  // add to journal entry
  if (le) 
    le->metablob.add_dir(dir, 
			 true,                 // Hmm: dirty=false would be okay in some cases
			 dir->is_complete());  

  int num_imported = 0;

  // take all waiters on this dir
  // NOTE: a pass of imported data is guaranteed to get all of my waiters because
  // a replica's presense in my cache implies/forces it's presense in authority's.
  list<Context*> waiters;
  
  dir->take_waiting(CDir::WAIT_ANY, waiters);
  for (list<Context*>::iterator it = waiters.begin();
       it != waiters.end();
       it++) 
    import_root->add_waiter(CDir::WAIT_UNFREEZE, *it);  // UNFREEZE will get kicked both on success or failure
  
  dout(15) << "doing contents" << dendl;
  
  // contents
  long nden;
  ::_decode_simple(nden, blp);
  
  for (; nden>0; nden--) {
    num_imported++;
    
    // dentry
    string dname;
    ::_decode_simple(dname, blp);
    
    CDentry *dn = dir->lookup(dname);
    if (!dn)
      dn = dir->add_null_dentry(dname);
    
    dn->decode_import(blp, ls);

    dn->add_replica(oldauth, CDentry::EXPORT_NONCE);
    if (dn->is_replica(mds->get_nodeid()))
      dn->remove_replica(mds->get_nodeid());

    dout(15) << "decode_import_dir got " << *dn << dendl;
    
    // points to...
    char icode;
    ::_decode_simple(icode, blp);
    
    if (icode == 'N') {
      // null dentry
      assert(dn->is_null());  
      
      // fall thru
    }
    else if (icode == 'L') {
      // remote link
      inodeno_t ino;
      unsigned char d_type;
      ::_decode_simple(ino, blp);
      ::_decode_simple(d_type, blp);
      if (dn->is_remote()) {
	assert(dn->get_remote_ino() == ino);
      } else {
	dir->link_remote_inode(dn, ino, d_type);
      }
    }
    else if (icode == 'I') {
      // inode
      decode_import_inode(dn, blp, oldauth, ls, cap_imports, updated_scatterlocks);
    }
    
    // add dentry to journal entry
    if (le)
      le->metablob.add_dentry(dn, dn->is_dirty());
  }
  
  dout(7) << "decode_import_dir done " << *dir << dendl;
  return num_imported;
}





// authority bystander

void Migrator::handle_export_notify(MExportDirNotify *m)
{
  CDir *dir = cache->get_dirfrag(m->get_dirfrag());

  int from = m->get_source().num();
  pair<int,int> old_auth = m->get_old_auth();
  pair<int,int> new_auth = m->get_new_auth();
  
  if (!dir) {
    dout(7) << "handle_export_notify " << old_auth << " -> " << new_auth
	    << " on missing dir " << m->get_dirfrag() << dendl;
  } else if (dir->authority() != old_auth) {
    dout(7) << "handle_export_notify old_auth was " << dir->authority() 
	    << " != " << old_auth << " -> " << new_auth
	    << " on " << *dir << dendl;
  } else {
    dout(7) << "handle_export_notify " << old_auth << " -> " << new_auth
	    << " on " << *dir << dendl;
    // adjust auth
    set<CDir*> have;
    cache->map_dirfrag_set(m->get_bounds(), have);
    cache->adjust_bounded_subtree_auth(dir, have, new_auth);
    
    // induce a merge?
    cache->try_subtree_merge(dir);
  }
  
  // send ack
  if (m->wants_ack()) {
    mds->send_message_mds(new MExportDirNotifyAck(m->get_dirfrag()), from);
  } else {
    // aborted.  no ack.
    dout(7) << "handle_export_notify no ack requested" << dendl;
  }
  
  delete m;
}








/** cap exports **/



void Migrator::export_caps(CInode *in)
{
  int dest = in->authority().first;
  dout(7) << "export_caps to mds" << dest << " " << *in << dendl;

  assert(in->is_any_caps());
  assert(!in->is_auth());
  assert(!in->is_ambiguous_auth());
  assert(!in->state_test(CInode::STATE_EXPORTINGCAPS));

  MExportCaps *ex = new MExportCaps;
  ex->ino = in->ino();

  encode_export_inode_caps(in, ex->cap_bl, ex->client_map);

  mds->send_message_mds(ex, dest);
}

void Migrator::handle_export_caps_ack(MExportCapsAck *ack)
{
  CInode *in = cache->get_inode(ack->ino);
  assert(in);
  dout(10) << "handle_export_caps_ack " << *ack << " from " << ack->get_source() 
	   << " on " << *in
	   << dendl;
  
  finish_export_inode_caps(in);
  delete ack;
}


class C_M_LoggedImportCaps : public Context {
  Migrator *migrator;
  CInode *in;
  int from;
public:
  map<CInode*, map<int,Capability::Export> > cap_imports;

  C_M_LoggedImportCaps(Migrator *m, CInode *i, int f) : migrator(m), in(i), from(f) {}
  void finish(int r) {
    migrator->logged_import_caps(in, from, cap_imports);
  }  
};

void Migrator::handle_export_caps(MExportCaps *ex)
{
  dout(10) << "handle_export_caps " << *ex << " from " << ex->get_source() << dendl;
  CInode *in = cache->get_inode(ex->ino);
  
  assert(in->is_auth());
  /*
   * note: i may be frozen, but i won't have been encoded for export (yet)!
   *  see export_go() vs export_go_synced().
   */

  C_M_LoggedImportCaps *finish = new C_M_LoggedImportCaps(this, in, ex->get_source().num());
  ESessions *le = new ESessions(++mds->sessionmap.projected);

  // decode new caps
  bufferlist::iterator blp = ex->cap_bl.begin();
  decode_import_inode_caps(in, blp, finish->cap_imports);
  assert(!finish->cap_imports.empty());   // thus, inode is pinned.
  
  // journal open client sessions
  mds->server->prepare_force_open_sessions(ex->client_map);
  le->client_map.swap(ex->client_map);
  
  mds->mdlog->submit_entry(le, finish);

  delete ex;
}


void Migrator::logged_import_caps(CInode *in, 
				  int from,
				  map<CInode*, map<int,Capability::Export> >& cap_imports) 
{
  dout(10) << "logged_import_caps on " << *in << dendl;
  assert(cap_imports.count(in));
  finish_import_inode_caps(in, from, cap_imports[in]);  

  mds->send_message_mds(new MExportCapsAck(in->ino()), from);
}





