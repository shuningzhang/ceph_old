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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "config.h"

#include "mon/MonMap.h"
#include "mon/Monitor.h"

#include "msg/SimpleMessenger.h"

#include "common/Timer.h"


int main(int argc, const char **argv) 
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  parse_config_options(args);

  // args
  int whoami = -1;
  const char *monmap_fn = ".ceph_monmap";
  for (unsigned i=0; i<args.size(); i++) {
    if (strcmp(args[i], "--mon") == 0) 
      whoami = atoi(args[++i]);
    else if (strcmp(args[i], "--monmap") == 0) 
      monmap_fn = args[++i];
    else {
      cerr << "unrecognized arg " << args[i] << std::endl;
      return -1;
    }
  }

  if (g_conf.clock_tare) g_clock.tare();

  MonMap monmap;

  if (whoami < 0) {
    // let's assume a standalone monitor
    whoami = 0;

    // start messenger
    rank.bind();
    cout << "starting standalone mon0, bound to " << rank.get_rank_addr() << std::endl;

    // add single mon0
    entity_inst_t inst;
    inst.name = entity_name_t::MON(0);
    inst.addr = rank.rank_addr;
    monmap.add_mon(inst);
    
    // write monmap
    cout << "writing monmap to " << monmap_fn << std::endl;;
    int r = monmap.write(monmap_fn);
    assert(r >= 0);
  } else {
    // i am specific monitor.

    // read monmap
    //cout << "reading monmap from " << monmap_fn << std::endl;
    int r = monmap.read(monmap_fn);
    if (r < 0) {
      cerr << "couldn't read monmap from " << monmap_fn << std::endl;
      return -1;
    }

    // bind to a specific port
    cout << "starting mon" << whoami << " at " << monmap.get_inst(whoami).addr
	 << " from " << monmap_fn
	 << std::endl;
    g_my_addr = monmap.get_inst(whoami).addr;
    rank.bind();
  }

  create_courtesy_output_symlink("mon", whoami);
  
  rank.start();

  // start monitor
  Messenger *m = rank.register_entity(entity_name_t::MON(whoami));
  Monitor *mon = new Monitor(whoami, m, &monmap);
  mon->init();

  rank.wait();

  // done
  delete mon;

  return 0;
}

