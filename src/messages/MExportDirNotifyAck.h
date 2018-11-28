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

#ifndef __MEXPORTDIRNOTIFYACK_H
#define __MEXPORTDIRNOTIFYACK_H

#include "msg/Message.h"
#include <string>
using namespace std;

class MExportDirNotifyAck : public Message {
  dirfrag_t dirfrag;

 public:
  dirfrag_t get_dirfrag() { return dirfrag; }
  
  MExportDirNotifyAck() {}
  MExportDirNotifyAck(dirfrag_t dirfrag) :
    Message(MSG_MDS_EXPORTDIRNOTIFYACK) {
    this->dirfrag = dirfrag;
  }
  const char *get_type_name() { return "ExNotA"; }
  void print(ostream& o) {
    o << "export_notify_ack(" << dirfrag << ")";
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
