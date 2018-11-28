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



#include <string>

#include "LogType.h"
#include "Logger.h"

#include <iostream>
#include "Clock.h"

#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>

#include "common/Timer.h"

// per-process lock.  lame, but this way I protect LogType too!
Mutex logger_lock;
SafeTimer logger_timer(logger_lock);
Context *logger_event = 0;
list<Logger*> logger_list;
utime_t start;
int last_flush; // in seconds since start

static void flush_all_loggers();

class C_FlushLoggers : public Context {
public:
  void finish(int r) {
    if (logger_event == this) {
      logger_event = 0;
      flush_all_loggers();
    }
  }
};

void Logger::set_start(utime_t s)
{
  logger_lock.Lock();

  start = s;

  utime_t fromstart = g_clock.now();
  if (fromstart < start) {
    cerr << "set_start: logger time jumped backwards from " << start << " to " << fromstart << std::endl;
    fromstart = start;
  }
  fromstart -= start;
  last_flush = fromstart.sec();

  logger_lock.Unlock();
}

static void flush_all_loggers()
{
  generic_dout(20) << "flush_all_loggers" << dendl;

  utime_t now = g_clock.now();
  utime_t fromstart = now;
  if (fromstart < start) {
    cerr << "logger time jumped backwards from " << start << " to " << fromstart << std::endl;
    //assert(0);
    start = fromstart;
  }
  fromstart -= start;
  int now_sec = fromstart.sec();

  // do any catching up we need to
  while (now_sec - last_flush >= g_conf.log_interval) {
    generic_dout(20) << "fromstart " << fromstart << " last_flush " << last_flush << " flushign" << dendl;
    for (list<Logger*>::iterator p = logger_list.begin();
	 p != logger_list.end();
	 ++p) 
      (*p)->_flush();
    last_flush += g_conf.log_interval;
  }

  // schedule next flush event
  utime_t next;
  next.sec_ref() = start.sec() + last_flush + g_conf.log_interval;
  next.usec_ref() = start.usec();
  generic_dout(20) << "logger now=" << now
		   << "  start=" << start 
		   << "  next=" << next 
		   << dendl;
  logger_event = new C_FlushLoggers;
  logger_timer.add_event_at(next, logger_event);
}



// ---------

Logger::Logger(string fn, LogType *type, bool append)
{
  logger_lock.Lock();
  {
    filename = "";
    if (g_conf.use_abspaths) {
      char cwd[200];
      getcwd(cwd, 200);
      filename = cwd;
      filename += "/";
    }

    filename = "log/";
    if (g_conf.log_name) {
      filename += g_conf.log_name;
      ::mkdir( filename.c_str(), 0755 );   // make sure dir exists
      filename += "/";
    }
    filename += fn;

    if (append)
      out.open(filename.c_str(), ofstream::out|ofstream::app);
    else
      out.open(filename.c_str(), ofstream::out);

    this->type = type;
    wrote_header = -1;
    wrote_header_last = 0;
    
    version = 0;

    if (logger_list.empty()) {
      // init logger
      if (!g_conf.clock_tare)
	start = g_clock.now();  // time 0!  otherwise g_clock does it for us.

      last_flush = 0;

      // call manually the first time; then it'll schedule itself.
      flush_all_loggers();      
    }
    logger_list.push_back(this);
  }
  logger_lock.Unlock();
}

Logger::~Logger()
{
  logger_lock.Lock();
  {
    _flush();
    out.close();
    logger_list.remove(this); // slow, but rare.
    if (logger_list.empty()) 
      logger_event = 0;       // stop the timer events.
  }
  logger_lock.Unlock();
}


/*
void Logger::flush()
{
  logger_lock.Lock();
  _flush();
  logger_lock.Unlock();
}
*/

void Logger::_flush()
{
  // header?
  wrote_header_last++;
  if (wrote_header != type->version ||
      wrote_header_last > 10) {
    out << "#" << type->keymap.size();
    for (unsigned i=0; i<type->keys.size(); i++) {
      out << "\t" << type->keys[i];
      if (type->avg[i]) 
	out << "\t" << type->keys[i] << "*\t" << type->keys[i] << "~";
    }
    out << std::endl;  //out << "\t (" << type->keymap.size() << ")" << endl;
    wrote_header = type->version;
    wrote_header_last = 0;
  }

  maybe_resize(type->keys.size());
  
  // write line to log
  out << last_flush;
  for (unsigned i=0; i<type->keys.size(); i++) {
    if (type->avg[i]) {
      if (vals[i] > 0) {
	double avg = (fvals[i] / (double)vals[i]);
	double var = 0.0;
	if (g_conf.logger_calc_variance) {
	  int n = vals[i];
	  for (vector<double>::iterator p = vals_to_avg[i].begin(); n--; ++p) 
	    var += (avg - *p) * (avg - *p);
	}
	out << "\t" << avg << "\t" << vals[i] << "\t" << var;
      } else
	out << "\t0\t0\t0";
    } else {
      if (fvals[i] > 0 && vals[i] == 0)
	out << "\t" << fvals[i];
      else {
	//cout << this << " p " << i << " and size is " << vals.size() << std::endl;
	out << "\t" << vals[i];
      }
    }
  }
  out << std::endl;
  
  // reset the counters
  for (unsigned i=0; i<type->keys.size(); i++) {
    if (type->inc_keys.count(i)) {
      this->vals[i] = 0;
      this->fvals[i] = 0;
    }
  }
}



long Logger::inc(const char *key, long v)
{
  if (!g_conf.log) return 0;
  logger_lock.Lock();
  int i = type->lookup_key(key);
  if (i < 0) i = type->add_inc(key);
  maybe_resize(i+1);

  vals[i] += v;
  long r = vals[i];
  logger_lock.Unlock();
  return r;
}

double Logger::finc(const char *key, double v)
{
  if (!g_conf.log) return 0;
  logger_lock.Lock();
  int i = type->lookup_key(key);
  if (i < 0) i = type->add_inc(key);
  maybe_resize(i+1);

  fvals[i] += v;
  double r = fvals[i];
  logger_lock.Unlock();
  return r;
}

long Logger::set(const char *key, long v)
{
  if (!g_conf.log) return 0;
  logger_lock.Lock();
  int i = type->lookup_key(key);
  if (i < 0) i = type->add_set(key);
  maybe_resize(i+1);

  //cout << this << " set " << i << " to " << v << std::endl;
  long r = vals[i] = v;
  logger_lock.Unlock();
  return r;
}


double Logger::fset(const char *key, double v)
{
  if (!g_conf.log) return 0;
  logger_lock.Lock();
  int i = type->lookup_key(key);
  if (i < 0) i = type->add_set(key);
  maybe_resize(i+1);

  //cout << this << " fset " << i << " to " << v << std::endl;
  double r = fvals[i] = v;
  logger_lock.Unlock();
  return r;
}

double Logger::favg(const char *key, double v)
{
  if (!g_conf.log) return 0;
  logger_lock.Lock();
  int i = type->lookup_key(key);
  if (i < 0) i = type->add_avg(key);
  maybe_resize(i+1);

  vals[i]++;
  double r = fvals[i] = v;
  if (g_conf.logger_calc_variance)
    vals_to_avg[i].push_back(v);
  logger_lock.Unlock();
  return r;
}

long Logger::get(const char* key)
{
  if (!g_conf.log) return 0;
  logger_lock.Lock();
  int i = type->lookup_key(key);
  maybe_resize(i+1);

  long r = 0;
  if (i >= 0 && i < (int)vals.size())
    r = vals[i];
  logger_lock.Unlock();
  return r;
}

