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

/* Journaler
 *
 * This class stripes a serial log over objects on the store.  Four logical pointers:
 *
 *  write_pos - where we're writing new entries
 *   read_pos - where we're reading old entires
 * expire_pos - what is deemed "old" by user
 *   trimmed_pos - where we're expiring old items
 *
 *  trimmed_pos <= expire_pos <= read_pos <= write_pos.
 *
 * Often, read_pos <= write_pos (as with MDS log).  During recovery, write_pos is undefined
 * until the end of the log is discovered.
 *
 * A "head" struct at the beginning of the log is used to store metadata at
 * regular intervals.  The basic invariants include:
 *
 *   head.read_pos   <= read_pos   -- the head may "lag", since it's updated lazily.
 *   head.write_pos  <= write_pos
 *   head.expire_pos <= expire_pos
 *   head.trimmed_pos   <= trimmed_pos
 *
 * More significantly,
 *
 *   head.expire_pos >= trimmed_pos -- this ensures we can find the "beginning" of the log
 *                                  as last recorded, before it is trimmed.  trimming will
 *                                  block until a sufficiently current expire_pos is committed.
 *
 * To recover log state, we simply start at the last write_pos in the head, and probe the
 * object sequence sizes until we read the end.  
 *
 * Head struct is stored in the first object.  Actual journal starts after layout.period() bytes.
 *
 */

#ifndef __JOURNALER_H
#define __JOURNALER_H

#include "Objecter.h"
#include "Filer.h"

#include <list>
#include <map>

class Context;
class Logger;

class Journaler {

  // this goes at the head of the log "file".
  struct Header {
    off_t trimmed_pos;
    off_t expire_pos;
    off_t read_pos;
    off_t write_pos;
    Header() : trimmed_pos(0), expire_pos(0), read_pos(0), write_pos(0) {}
  } last_written, last_committed;

  friend ostream& operator<<(ostream& out, Header &h);


  // me
  inode_t inode;
  Objecter *objecter;
  Filer filer;

  Logger *logger;

  Mutex *lock;
  SafeTimer timer;

  class C_DelayFlush : public Context {
    Journaler *journaler;
  public:
    C_DelayFlush(Journaler *j) : journaler(j) {}
    void finish(int r) {
      journaler->delay_flush_event = 0;
      journaler->_do_flush();
    }
  } *delay_flush_event;


  // my state
  static const int STATE_UNDEF = 0;
  static const int STATE_READHEAD = 1;
  static const int STATE_PROBING = 2;
  static const int STATE_ACTIVE = 2;

  int state;

  // header
  utime_t last_wrote_head;
  void _finish_write_head(Header &wrote, Context *oncommit);
  class C_WriteHead;
  friend class C_WriteHead;

  list<Context*> waitfor_recover;
  void _finish_read_head(int r, bufferlist& bl);
  void _finish_probe_end(int r, off_t end);
  class C_ReadHead;
  friend class C_ReadHead;
  class C_ProbeEnd;
  friend class C_ProbeEnd;



  // writer
  off_t write_pos;       // logical write position, where next entry will go
  off_t flush_pos;       // where we will flush. if write_pos>flush_pos, we're buffering writes.
  off_t ack_pos;         // what has been acked.
  bufferlist write_buf;  // write buffer.  flush_pos + write_buf.length() == write_pos.

  std::map<off_t, utime_t> pending_flush;  // start offsets and times for pending flushes
  std::map<off_t, std::list<Context*> > waitfor_flush; // when flushed through given offset

  void _do_flush();
  void _finish_flush(int r, off_t start);
  class C_Flush;
  friend class C_Flush;

  // reader
  off_t read_pos;      // logical read position, where next entry starts.
  off_t requested_pos; // what we've requested from OSD.
  off_t received_pos;  // what we've received from OSD.
  bufferlist read_buf; // read buffer.  read_pos + read_buf.length() == prefetch_pos.
  bufferlist reading_buf; // what i'm reading into

  off_t fetch_len;     // how much to read at a time
  off_t prefetch_from; // how far from end do we read next chunk

  // for read_entry() in-progress read
  bufferlist *read_bl;
  Context    *on_read_finish;
  // for wait_for_readable()
  Context    *on_readable;

  bool _is_reading() {
    return requested_pos > received_pos;
  }
  void _finish_read(int r);     // we just read some (read completion callback)
  void _issue_read(off_t len);  // read some more
  void _prefetch();             // maybe read ahead
  class C_Read;
  friend class C_Read;
  class C_RetryRead;
  friend class C_RetryRead;

  // trimmer
  off_t expire_pos;    // what we're allowed to trim to
  off_t trimming_pos;      // what we've requested to trim through
  off_t trimmed_pos;   // what has been trimmed
  map<off_t, list<Context*> > waitfor_trim;

  void _trim_finish(int r, off_t to);
  class C_Trim;
  friend class C_Trim;

public:
  Journaler(inode_t& inode_, Objecter *obj, Logger *l, Mutex *lk, off_t fl=0, off_t pff=0) : 
    inode(inode_), objecter(obj), filer(objecter), logger(l), 
    lock(lk), timer(*lk), delay_flush_event(0),
    state(STATE_UNDEF),
    write_pos(0), flush_pos(0), ack_pos(0),
    read_pos(0), requested_pos(0), received_pos(0),
    fetch_len(fl), prefetch_from(pff),
    read_bl(0), on_read_finish(0), on_readable(0),
    expire_pos(0), trimming_pos(0), trimmed_pos(0) 
  {
    // prefetch intelligently.
    // (watch out, this is big if you use big objects or weird striping)
    if (!fetch_len)
      fetch_len = inode.layout.fl_object_size*inode.layout.fl_stripe_count *
	g_conf.journaler_prefetch_periods;
    if (!prefetch_from)
      prefetch_from = fetch_len / 2;
  }

  // me
  //void open(Context *onopen);
  //void claim(Context *onclaim, msg_addr_t from);

  /* reset 
   *  NOTE: we assume the caller knows/has ensured that any objects 
   * in our sequence do not exist.. e.g. after a MKFS.  this is _not_
   * an "erase" method.
   */
  void reset();
  void recover(Context *onfinish);
  void write_head(Context *onsave=0);

  bool is_active() { return state == STATE_ACTIVE; }

  off_t get_write_pos() const { return write_pos; }
  off_t get_write_ack_pos() const { return ack_pos; }
  off_t get_read_pos() const { return read_pos; }
  off_t get_expire_pos() const { return expire_pos; }
  off_t get_trimmed_pos() const { return trimmed_pos; }

  // write
  off_t append_entry(bufferlist& bl, Context *onsync = 0);
  void flush(Context *onsync = 0);

  // read
  void set_read_pos(off_t p) { 
    assert(requested_pos == received_pos);  // we can't cope w/ in-progress read right now.
    assert(read_bl == 0); // ...
    read_pos = requested_pos = received_pos = p;
    read_buf.clear();
  }
  bool is_readable();
  bool try_read_entry(bufferlist& bl);
  void wait_for_readable(Context *onfinish);
  void read_entry(bufferlist* bl, Context *onfinish);
  
  // trim
  void set_expire_pos(off_t ep) { expire_pos = ep; }
  void trim();
  //bool is_trimmable() { return trimming_pos < expire_pos; }
  //void trim(off_t trim_to=0, Context *c=0);
};


#endif
