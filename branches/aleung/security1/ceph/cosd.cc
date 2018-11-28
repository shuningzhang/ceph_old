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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "config.h"

#include "mon/MonMap.h"

#include "osd/OSD.h"
#include "ebofs/Ebofs.h"

#include "msg/SimpleMessenger.h"

#include "common/Timer.h"


class C_Die : public Context {
public:
  void finish(int) {
    cerr << "die" << endl;
    exit(1);
  }
};

class C_Debug : public Context {
  public:
  void finish(int) {
    int size = &g_conf.debug_after - &g_conf.debug;
    memcpy((char*)&g_conf.debug, (char*)&g_debug_after_conf.debug, size);
    dout(0) << "debug_after flipping debug settings" << endl;
  }
};


int main(int argc, char **argv) 
{
  vector<char*> args;
  argv_to_vec(argc, argv, args);

  parse_config_options(args);

  if (g_conf.kill_after) 
    g_timer.add_event_after(g_conf.kill_after, new C_Die);
  if (g_conf.debug_after) 
    g_timer.add_event_after(g_conf.debug_after, new C_Debug);

  // osd specific args
  char *dev;
  int whoami = -1;
  for (unsigned i=0; i<args.size(); i++) {
    if (strcmp(args[i],"--dev") == 0) 
      dev = args[++i];
    else if (strcmp(args[i],"--osd") == 0)
      whoami = atoi(args[++i]);
    else {
      cerr << "unrecognized arg " << args[i] << endl;
      return -1;
    }
  }
  cout << "dev " << dev << endl;
  

  if (whoami < 0) {
    // who am i?   peek at superblock!
    OSDSuperblock sb;
    ObjectStore *store = new Ebofs(dev);
    bufferlist bl;
    store->mount();
    int r = store->read(object_t(0,0), 0, sizeof(sb), bl);
    if (r < 0) {
      cerr << "couldn't read superblock object on " << dev << endl;
      exit(0);
    }
    bl.copy(0, sizeof(sb), (char*)&sb);
    store->umount();
    delete store;
    whoami = sb.whoami;
    
    cout << "osd fs says i am osd" << whoami << endl;
  } else {
    cout << "command line arg says i am osd" << whoami << endl;
  }

  // load monmap
  MonMap monmap;
  int r = monmap.read(".ceph_monmap");
  assert(r >= 0);

  // start up network
  rank.start_rank();

  // start osd
  Messenger *m = rank.register_entity(MSG_ADDR_OSD(whoami));
  assert(m);
  OSD *osd = new OSD(whoami, m, &monmap, dev);
  osd->init();

  // wait
  rank.wait();

  // done
  delete osd;

  return 0;
}

