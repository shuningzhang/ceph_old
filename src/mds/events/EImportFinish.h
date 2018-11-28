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

#ifndef __EIMPORTFINISH_H
#define __EIMPORTFINISH_H

#include <assert.h>
#include "config.h"
#include "include/types.h"

#include "../MDS.h"

class EImportFinish : public LogEvent {
 protected:
  dirfrag_t base; // imported dir
  bool success;

 public:
  EImportFinish(CDir *dir, bool s) : LogEvent(EVENT_IMPORTFINISH), 
				     base(dir->dirfrag()),
				     success(s) { }
  EImportFinish() : LogEvent(EVENT_IMPORTFINISH) { }
  
  void print(ostream& out) {
    out << "EImportFinish " << base;
    if (success)
      out << " success";
    else
      out << " failed";
  }

  virtual void encode_payload(bufferlist& bl) {
    bl.append((char*)&base, sizeof(base));
    bl.append((char*)&success, sizeof(success));
  }
  void decode_payload(bufferlist& bl, int& off) {
    bl.copy(off, sizeof(base), (char*)&base);
    off += sizeof(base);
    bl.copy(off, sizeof(success), (char*)&success);
    off += sizeof(success);
  }
  
  void replay(MDS *mds);

};

#endif
