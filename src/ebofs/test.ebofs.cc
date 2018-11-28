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



#include <iostream>
#include "ebofs/Ebofs.h"

bool stop = false;


char fingerprint_byte_at(int pos, int seed)
{
  __u64 big = ((pos & ~7) / 133) ^ seed;
  return ((char*)&big)[pos & 7];
}


int nt = 0;
class Tester : public Thread {
  Ebofs &fs;
  int t;
  
  //char b[1024*1024];

public:
  Tester(Ebofs &e) : fs(e), t(nt) { nt++; }
  void *entry() {

    while (!stop) {
      object_t oid;
      oid.ino = (rand() % 1000) + 0x10000000;
      coll_t cid = rand() % 50;
      off_t off = rand() % 10000;//0;//rand() % 1000000;
      off_t len = 1+rand() % 100000;
      const char *a = "one";
      if (rand() % 2) a = "two";
      int l = 3;//rand() % 10;

      switch (rand() % 5) {//10) {
      case 0:
        {
	  oid.rev = rand() % 10;
          cout << t << " read " << hex << oid << dec << " at " << off << " len " << len << std::endl;
          bufferlist bl;
          fs.read(oid, off, len, bl);
          int l = MIN(len,bl.length());
          if (l) {
            cout << t << " got " << l << std::endl;
            char *p = bl.c_str();
            while (l--) {
	      char want = fingerprint_byte_at(off, oid.ino);
              if (*p != 0 && *p != want) {
		cout << t << " bad fingerprint at " << off << " got " << (int)*p << " want " << (int)want << std::endl;
		assert(0);
	      }
              off++;
              p++;
            }
          }
        }
        break;

      case 1:
        {
          cout << t << " write " << hex << oid << dec << " at " << off << " len " << len << std::endl;
	  char b[len];
          for (int j=0;j<len;j++)
            b[j] = fingerprint_byte_at(off+j, oid.ino);
          bufferlist w;
	  w.append(b, len);
          fs.write(oid, off, len, w, 0);
        }
        break;

      case 2:
        {
          cout << t << " zero " << hex << oid << dec << " at " << off << " len " << len << std::endl;
          fs.zero(oid, off, len, 0);
        }
        break;

      case 3:
        {
          cout << t << " truncate " << hex << oid << dec <<  " " << off << std::endl;
          fs.truncate(oid, 0);
        }
        break;

      case 4:
        cout << t << " remove " << hex << oid << dec <<  std::endl;
        fs.remove(oid);
        break;

      case 5:
        cout << t << " collection_add " << hex << oid << dec <<  " to " << cid << std::endl;
        fs.collection_add(cid, oid, 0);
        break;

      case 6:
        cout << t << " collection_remove " << hex << oid << dec <<  " from " << cid << std::endl;
        fs.collection_remove(cid, oid, 0);
        break;

      case 7:
        cout << t << " setattr " << hex << oid << dec <<  " " << a << " len " << l << std::endl;
        fs.setattr(oid, a, (void*)a, l, 0);
        break;
        
      case 8:
        cout << t << " rmattr " << hex << oid << dec <<  " " << a << std::endl;
        fs.rmattr(oid,a);
        break;

      case 9:
        {
          char v[4];
          cout << t << " getattr " << hex << oid << dec <<  " " << a << std::endl;
          if (fs.getattr(oid,a,(void*)v,3) == 0) {
            v[3] = 0;
            assert(strcmp(v,a) == 0);
          }
        }
        break;
        
      case 10:
	{
	  object_t newoid = oid;
	  newoid.rev = rand() % 10;
	  cout << t << " clone " << oid << " to " << newoid << std::endl;
	  fs.clone(oid, newoid, 0);
	}
      }


    }
    cout << t << " done" << std::endl;
    return 0;
  }
};

int main(int argc, const char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  parse_config_options(args);

  // args
  if (args.size() != 3) return -1;
  const char *filename = args[0];
  int seconds = atoi(args[1]);
  int threads = atoi(args[2]);
  if (!threads) threads = 1;

  cout << "dev " << filename << " .. " << threads << " threads .. " << seconds << " seconds" << std::endl;

  Ebofs fs(filename);
  if (fs.mount() < 0) return -1;


  // explicit tests
  if (0) {
    // verify that clone() plays nice with partial writes
    object_t oid(1,1);
    bufferptr bp(10000);
    bp.zero();
    bufferlist bl;
    bl.push_back(bp);
    fs.write(oid, 0, 10000, bl, 0);

    fs.sync();
    fs.trim_buffer_cache();

    // induce a partial write
    bufferlist bl2;
    bl2.substr_of(bl, 0, 100);
    fs.write(oid, 100, 100, bl2, 0);

    // clone it
    object_t oid2;
    oid2 = oid;
    oid2.rev = 1;
    fs.clone(oid, oid2, 0);

    // ... 
    if (0) {
      // make sure partial still behaves after orig is removed...
      fs.remove(oid, 0);

      // or i read for oid2...
      bufferlist rbl;
      fs.read(oid2, 0, 200, rbl);
    }
    if (1) {
      // make sure things behave if we remove the clone
      fs.remove(oid2,0);
    }
  }
  // /explicit tests

  list<Tester*> ls;
  for (int i=0; i<threads; i++) {
    Tester *t = new Tester(fs);
    t->create();
    ls.push_back(t);
  }

  utime_t now = g_clock.now();
  utime_t dur(seconds, 0);
  utime_t end = now + dur;
  cout << "stop at " << end << std::endl;
  while (now < end) {
    sleep(1);
    now = g_clock.now();
    //cout << now << std::endl;
  }

  cout << "stopping" << std::endl;
  stop = true;
  
  while (!ls.empty()) {
    Tester *t = ls.front();
    ls.pop_front();
    t->join();
    delete t;
  }

  fs.umount();
  return 0;
}

