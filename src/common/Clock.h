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



#ifndef __CLOCK_H
#define __CLOCK_H

#include <iostream>
#include <iomanip>

#include <sys/time.h>
#include <time.h>

#include "Mutex.h"

#include "include/utime.h"



// -- clock --
class Clock {
 protected:
  //utime_t start_offset;
  //utime_t abs_last;
  utime_t last;
  utime_t zero;

  Mutex lock;

 public:
  Clock() {
    // set offset
    //tare();
  }

  // real time.
  utime_t real_now() {
    utime_t realnow = now();
    realnow += zero;
    //gettimeofday(&realnow.timeval(), NULL);
    return realnow;
  }

  // relative time (from startup)
  void tare() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    zero.set_from_timeval(&tv);
  }
  void tare(utime_t z) {
    zero = z;
  }
  utime_t now() {
    //lock.Lock();  
    struct timeval tv;
    gettimeofday(&tv, NULL);
    utime_t n(&tv);
    n -= zero;
    if (n < last) {
      //std::cerr << "WARNING: clock jumped backwards from " << last << " to " << n << std::endl;
      n = last;    // clock jumped backwards!
    } else
      last = n;
    //lock.Unlock();
    return n;
  }
  utime_t recent_now() {
    return last;
  }

  void realify(utime_t& t) {
    t += zero;
  }

  void make_timespec(utime_t& t, struct timespec *ts) {
    utime_t real = t;
    realify(real);

    memset(ts, 0, sizeof(*ts));
    ts->tv_sec = real.sec();
    ts->tv_nsec = real.nsec();
  }



  // absolute time
  time_t gettime() {
    return real_now().sec();
  }

};

extern Clock g_clock;

#endif
