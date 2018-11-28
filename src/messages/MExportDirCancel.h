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

#ifndef __MEXPORTDIRCANCEL_H
#define __MEXPORTDIRCANCEL_H

#include "msg/Message.h"
#include "mds/CInode.h"
#include "include/types.h"

class MExportDirCancel : public Message {
  dirfrag_t dirfrag;

 public:
  dirfrag_t get_dirfrag() { return dirfrag; }

  MExportDirCancel() {}
  MExportDirCancel(dirfrag_t df) : 
    Message(MSG_MDS_EXPORTDIRCANCEL),
	dirfrag(df) { }

  const char *get_type_name() { return "ExCancel"; }
  void print(ostream& o) {
    o << "export_cancel(" << dirfrag << ")";
  }

  virtual void decode_payload() {
    int off = 0;
    payload.copy(off, sizeof(dirfrag), (char*)&dirfrag);
    off += sizeof(dirfrag);
  }

  virtual void encode_payload() {
    payload.append((char*)&dirfrag, sizeof(dirfrag));
  }
};

#endif
