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


#ifndef __EBOFS_FILEJOURNAL_H
#define __EBOFS_FILEJOURNAL_H


#include "Journal.h"
#include "common/Cond.h"
#include "common/Mutex.h"
#include "common/Thread.h"

class FileJournal : public Journal {
public:
  /** log header
   * we allow 4 pointers:
   *  top/initial,
   *  one for an epoch boundary (if any),
   *  one for a wrap in the ring buffer/journal file,
   *  one for a second epoch boundary (if any).
   * the epoch boundary one is useful only for speedier recovery in certain cases
   * (i.e. when ebofs committed, but the journal didn't rollover ... very small window!)
   */
  struct header_t {
    __u64 fsid;
    __s64 num;
    __u32 block_size;
    __u32 alignment;
    __s64 max_size;
    __s64 wrap;
    __u32 epoch[4];
    __s64 offset[4];

    header_t() : fsid(0), num(0), block_size(0), alignment(0), max_size(0), wrap(0) {}

    void clear() {
      num = 0;
      wrap = 0;
    }
    void pop() {
      if (num >= 2 && offset[0] > offset[1]) 
	wrap = 0;  // we're eliminating a wrap
      num--;
      for (int i=0; i<num; i++) {
	epoch[i] = epoch[i+1];
	offset[i] = offset[i+1];
      }
    }
    void push(epoch_t e, off64_t o) {
      assert(num < 4);
      if (num > 2 && 
	  epoch[num-1] == e &&
	  epoch[num-2] == (e-1)) 
	num--;  // tail was an epoch boundary; replace it.
      epoch[num] = e;
      offset[num] = o;
      num++;
    }
    epoch_t last_epoch() {
      if (num)
	return epoch[num-1];
      else
	return 0;
    }
  } header;

  struct entry_header_t {
    uint64_t epoch;
    uint64_t len;
    uint64_t magic1;
    uint64_t magic2;
    
    void make_magic(off64_t pos, uint64_t fsid) {
      magic1 = pos;
      magic2 = fsid ^ epoch ^ len;
    }
    bool check_magic(off64_t pos, uint64_t fsid) {
      return
	magic1 == (uint64_t)pos &&
	magic2 == (fsid ^ epoch ^ len);
    }
  };

private:
  string fn;

  off64_t max_size;
  size_t block_size;
  bool directio;
  bool full, writing, must_write_header;
  off64_t write_pos;      // byte where next entry written goes
  off64_t read_pos;       // 

  int fd;

  // to be journaled
  list<pair<epoch_t,bufferlist> > writeq;
  list<Context*> commitq;

  // being journaled
  list<Context*> writingq;
  
  // write thread
  Mutex write_lock;
  Cond write_cond;
  bool write_stop;

  int _open(bool wr);
  void print_header();
  void read_header();
  bufferptr prepare_header();
  void start_writer();
  void stop_writer();
  void write_thread_entry();

  void check_for_wrap(epoch_t epoch, off64_t pos, off64_t size);
  bool prepare_single_dio_write(bufferlist& bl);
  void prepare_multi_write(bufferlist& bl);
  void do_write(bufferlist& bl);

  class Writer : public Thread {
    FileJournal *journal;
  public:
    Writer(FileJournal *fj) : journal(fj) {}
    void *entry() {
      journal->write_thread_entry();
      return 0;
    }
  } write_thread;

  off64_t get_top() {
    if (directio)
      return block_size;
    else
      return sizeof(header);
  }

 public:
  FileJournal(Ebofs *e, const char *f, bool dio=false) : 
    Journal(e), fn(f),
    max_size(0), block_size(0),
    directio(dio),
    full(false), writing(false), must_write_header(false),
    write_pos(0), read_pos(0),
    fd(-1),
    write_stop(false), write_thread(this) { }
  ~FileJournal() {}

  int create();
  int open();
  void close();

  void make_writeable();

  // writes
  void submit_entry(bufferlist& e, Context *oncommit);  // submit an item
  void commit_epoch_start();   // mark epoch boundary
  void commit_epoch_finish(epoch_t);  // mark prior epoch as committed (we can expire)

  bool read_entry(bufferlist& bl, epoch_t& e);

  bool is_full();

  // reads
};

#endif
