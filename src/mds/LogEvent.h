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

#ifndef __LOGEVENT_H
#define __LOGEVENT_H

#define EVENT_STRING       1

#define EVENT_SUBTREEMAP   2
#define EVENT_EXPORT       3
#define EVENT_IMPORTSTART  4
#define EVENT_IMPORTFINISH 5
#define EVENT_FRAGMENT     6

#define EVENT_SESSION      10
#define EVENT_SESSIONS     11

#define EVENT_UPDATE       20
#define EVENT_SLAVEUPDATE  21
#define EVENT_OPEN         22

#define EVENT_PURGEFINISH  30

#define EVENT_ANCHOR       40
#define EVENT_ANCHORCLIENT 41




#include <string>
using namespace std;

#include "include/buffer.h"
#include "include/Context.h"

class MDS;
class LogSegment;

// generic log event
class LogEvent {
 private:
  int _type;
  off_t _start_off,_end_off;

  friend class MDLog;

 public:
  LogSegment *_segment;

  LogEvent(int t) : 
    _type(t), _start_off(0), _end_off(0), _segment(0) { }
  virtual ~LogEvent() { }

  int get_type() { return _type; }
  off_t get_start_off() { return _start_off; }
  off_t get_end_off() { return _end_off; }

  // encoding
  virtual void encode_payload(bufferlist& bl) = 0;
  virtual void decode_payload(bufferlist& bl, int& off) = 0;
  static LogEvent *decode(bufferlist &bl);


  virtual void print(ostream& out) { 
    out << "event(" << _type << ")";
  }

  /*** live journal ***/
  /* update_segment() - adjust any state we need to in the LogSegment 
   */
  virtual void update_segment() { }

  /*** recovery ***/
  /* replay() - replay given event.  this is idempotent.
   */
  virtual void replay(MDS *m) { assert(0); }


};

inline ostream& operator<<(ostream& out, LogEvent& le) {
  le.print(out);
  return out;
}

#endif
