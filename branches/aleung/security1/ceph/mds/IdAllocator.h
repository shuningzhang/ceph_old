// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
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


#ifndef __IDALLOCATOR_H
#define __IDALLOCATOR_H

#include "include/types.h"
#include "include/interval_set.h"
#include "include/buffer.h"
#include "include/Context.h"

class MDS;

#define IDTYPE_INO 1
typedef inodeno_t idno_t;

class IdAllocator {
  MDS *mds;
  inode_t inode;

  static const int STATE_UNDEF   = 0;
  static const int STATE_OPENING = 1;
  static const int STATE_ACTIVE  = 2;
  //static const int STATE_COMMITTING = 3;
  int state;

  version_t version, committing_version, committed_version;

  interval_set<idno_t> free;   // unused ids
  
  map<version_t, list<Context*> > waitfor_save;

 public:
  IdAllocator(MDS *m) :
    mds(m),
    state(STATE_UNDEF),
    version(0), committing_version(0), committed_version(0)
  {
  }
  
  void init_inode();

  // alloc or reclaim ids
  idno_t alloc_id(bool replay=false);
  void reclaim_id(idno_t id, bool replay=false);

  version_t get_version() { return version; }
  version_t get_committed_version() { return committed_version; }

  // load/save from disk (hack)
  bool is_undef() { return state == STATE_UNDEF; }
  bool is_active() { return state == STATE_ACTIVE; }
  bool is_opening() { return state == STATE_OPENING; }

  void reset();
  void save(Context *onfinish=0, version_t need=0);
  void save_2(version_t v);

  void shutdown() {
    if (is_active()) save(0);
  }

  void load(Context *onfinish);
  void load_2(int, bufferlist&, Context *onfinish);

};

#endif
