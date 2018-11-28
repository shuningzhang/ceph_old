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


#ifndef __MSTATFSREPLY_H
#define __MSTATFSREPLY_H

class MStatfsReply : public Message {
public:
  tid_t tid;
  struct ceph_statfs stfs;

  MStatfsReply() : Message(CEPH_MSG_STATFS_REPLY) {}
  MStatfsReply(tid_t t) : Message(CEPH_MSG_STATFS_REPLY), tid(t) {}

  const char *get_type_name() { return "statfs_reply"; }
  void print(ostream& out) {
    out << "statfs_reply(" << tid << ")";
  }

  void encode_payload() {
    ::_encode(tid, payload);
    ::_encode(stfs, payload);
  }
  void decode_payload() {
    int off = 0;
    ::_decode(tid, payload, off);
    ::_decode(stfs, payload, off);
  }
};

#endif
