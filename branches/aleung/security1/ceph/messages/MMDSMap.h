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


#ifndef __MMDSMAP_H
#define __MMDSMAP_H

#include "msg/Message.h"
#include "mds/MDSMap.h"


class MMDSMap : public Message {
 public:
  /*
  map<epoch_t, bufferlist> maps;
  map<epoch_t, bufferlist> incremental_maps;

  epoch_t get_first() {
    epoch_t e = 0;
    map<epoch_t, bufferlist>::iterator i = maps.begin();
    if (i != maps.end())  e = i->first;
    i = incremental_maps.begin();    
    if (i != incremental_maps.end() &&
        (e == 0 || i->first < e)) e = i->first;
    return e;
  }
  epoch_t get_last() {
    epoch_t e = 0;
    map<epoch_t, bufferlist>::reverse_iterator i = maps.rbegin();
    if (i != maps.rend())  e = i->first;
    i = incremental_maps.rbegin();    
    if (i != incremental_maps.rend() &&
        (e == 0 || i->first > e)) e = i->first;
    return e;
  }
  */

  version_t epoch;
  bufferlist encoded;

  version_t get_epoch() const { return epoch; }
  bufferlist& get_encoded() { return encoded; }

  MMDSMap() : 
    Message(MSG_MDS_MAP) {}
  MMDSMap(MDSMap *mm) :
    Message(MSG_MDS_MAP) {
    epoch = mm->get_epoch();
    mm->encode(encoded);
  }


  // marshalling
  virtual void decode_payload() {
    int off = 0;
    payload.copy(off, sizeof(epoch), (char*)&epoch);
    off += sizeof(epoch);
    ::_decode(encoded, payload, off);
  }
  virtual void encode_payload() {
    payload.append((char*)&epoch, sizeof(epoch));
    ::_encode(encoded, payload);
  }

  virtual char *get_type_name() { return "mdsmap"; }
};

#endif
