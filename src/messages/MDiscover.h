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


#ifndef __MDISCOVER_H
#define __MDISCOVER_H

#include "msg/Message.h"
#include "mds/CDir.h"
#include "include/filepath.h"

#include <vector>
#include <string>
using namespace std;


class MDiscover : public Message {
  int             asker;
  inodeno_t       base_ino;          // 1 -> root
  frag_t          base_dir_frag;

  filepath        want;   // ... [/]need/this/stuff
  inodeno_t       want_ino;

  bool want_base_dir;
  bool want_xlocked;

 public:
  int       get_asker() { return asker; }
  inodeno_t get_base_ino() { return base_ino; }
  frag_t    get_base_dir_frag() { return base_dir_frag; }

  filepath& get_want() { return want; }
  inodeno_t get_want_ino() { return want_ino; }
  const string& get_dentry(int n) { return want[n]; }

  bool wants_base_dir() { return want_base_dir; }
  bool wants_xlocked() { return want_xlocked; }
  
  void set_base_dir_frag(frag_t f) { base_dir_frag = f; }

  MDiscover() { }
  MDiscover(int asker_, 
            inodeno_t base_ino_,
            filepath& want_,
            bool want_base_dir_ = true,
	    bool discover_xlocks_ = false) :
    Message(MSG_MDS_DISCOVER),
    asker(asker_),
    base_ino(base_ino_),
    want(want_),
    want_ino(0),
    want_base_dir(want_base_dir_),
    want_xlocked(discover_xlocks_) { }
  MDiscover(int asker_, 
            dirfrag_t base_dirfrag,
            inodeno_t want_ino_,
            bool want_base_dir_ = true) :
    Message(MSG_MDS_DISCOVER),
    asker(asker_),
    base_ino(base_dirfrag.ino),
    base_dir_frag(base_dirfrag.frag),
    want_ino(want_ino_),
    want_base_dir(want_base_dir_),
    want_xlocked(false) { }

  const char *get_type_name() { return "Dis"; }
  void print(ostream &out) {
    out << "discover(" << base_ino << "." << base_dir_frag
	<< " " << want;
    if (want_ino) out << want_ino;
    out << ")";
  }

  void decode_payload() {
    int off = 0;
    ::_decode(asker, payload, off);
    ::_decode(base_ino, payload, off);
    ::_decode(base_dir_frag, payload, off);
    want._decode(payload, off);
    ::_decode(want_ino, payload, off);
    ::_decode(want_base_dir, payload, off);
    ::_decode(want_xlocked, payload, off);
  }
  void encode_payload() {
    ::_encode(asker, payload);
    ::_encode(base_ino, payload);
    ::_encode(base_dir_frag, payload);
    want._encode(payload);
    ::_encode(want_ino, payload);
    ::_encode(want_base_dir, payload);
    ::_encode(want_xlocked, payload);
  }

};

#endif
