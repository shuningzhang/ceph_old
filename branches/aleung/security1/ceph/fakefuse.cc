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



#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "config.h"

#include "mon/Monitor.h"

#include "mds/MDS.h"
#include "osd/OSD.h"
#include "client/Client.h"
#include "client/fuse.h"

#include "common/Timer.h"

#include "msg/FakeMessenger.h"




#define NUMMDS g_conf.num_mds
#define NUMOSD g_conf.num_osd
#define NUMCLIENT g_conf.num_client


class C_Test : public Context {
public:
  void finish(int r) {
    cout << "C_Test->finish(" << r << ")" << endl;
  }
};
class C_Test2 : public Context {
public:
  void finish(int r) {
    cout << "C_Test2->finish(" << r << ")" << endl;
    g_timer.add_event_after(2, new C_Test);
  }
};



int main(int argc, char **argv) {
  cerr << "fakefuse starting" << endl;

  vector<char*> args;
  argv_to_vec(argc, argv, args);
  parse_config_options(args);

  // start messenger thread
  fakemessenger_startthread();

  //g_timer.add_event_after(5.0, new C_Test2);
  //g_timer.add_event_after(10.0, new C_Test);

  vector<char*> nargs;
  for (unsigned i=0; i<args.size(); i++) {
    nargs.push_back(args[i]);
  }
  args = nargs;
  vec_to_argv(args, argc, argv);

  // FUSE will chdir("/"); be ready.
  g_conf.use_abspaths = true;

  MonMap *monmap = new MonMap(g_conf.num_mon);
  
  Monitor *mon[g_conf.num_mon];
  for (int i=0; i<g_conf.num_mon; i++) {
    mon[i] = new Monitor(i, new FakeMessenger(MSG_ADDR_MON(i)), monmap);
  }

  // create osd
  OSD *osd[NUMOSD];
  for (int i=0; i<NUMOSD; i++) {
    osd[i] = new OSD(i, new FakeMessenger(MSG_ADDR_OSD(i)), monmap);
  }

  // create mds
  MDS *mds[NUMMDS];
  for (int i=0; i<NUMMDS; i++) {
    mds[i] = new MDS(i, new FakeMessenger(MSG_ADDR_MDS(i)), monmap);
  }
 
    // init
  for (int i=0; i<g_conf.num_mon; i++) {
    mon[i]->init();
  }
  for (int i=0; i<NUMMDS; i++) {
    mds[i]->init();
  }
  
  for (int i=0; i<NUMOSD; i++) {
    osd[i]->init();
  }


  // create client
  Client *client[NUMCLIENT];
  for (int i=0; i<NUMCLIENT; i++) {
    client[i] = new Client(new FakeMessenger(MSG_ADDR_CLIENT(0)), monmap);
    client[i]->init();


    // start up fuse
    // use my argc, argv (make sure you pass a mount point!)
    cout << "starting fuse on pid " << getpid() << endl;
    client[i]->mount();
    ceph_fuse_main(client[i], argc, argv);
    client[i]->unmount();
    cout << "fuse finished on pid " << getpid() << endl;
    client[i]->shutdown();
  }
  


  // wait for it to finish
  cout << "DONE -----" << endl;
  fakemessenger_wait();  // blocks until messenger stops
  

  // cleanup
  for (int i=0; i<NUMMDS; i++) {
    delete mds[i];
  }
  for (int i=0; i<NUMOSD; i++) {
    delete osd[i];
  }
  for (int i=0; i<NUMCLIENT; i++) {
    delete client[i];
  }
  
  return 0;
}

