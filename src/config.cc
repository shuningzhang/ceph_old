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


#include "config.h"
#include "include/types.h"
#include <fstream>

//#define MDS_CACHE_SIZE        4*10000   -> <20mb
//#define MDS_CACHE_SIZE        80000         62mb

#define AVG_PER_INODE_SIZE    450
#define MDS_CACHE_MB_TO_INODES(x) ((x)*1000000/AVG_PER_INODE_SIZE)

//#define MDS_CACHE_SIZE       MDS_CACHE_MB_TO_INODES( 50 )
//#define MDS_CACHE_SIZE 1500000
#define MDS_CACHE_SIZE 150000


// hack hack hack ugly FIXME
#include "include/atomic.h"
atomic_t buffer_total_alloc;

#include "osd/osd_types.h"

// debug output
Mutex _dout_lock;
ostream *_dout = &std::cout;
ostream *_derr = &std::cerr;
char _dout_file[100] = {0};
char _dout_dir[1000] = {0};
char _dout_symlink_path[1000] = {0};

// page size crap, see page.h
int _get_bits_of(int v) {
  int n = 0;
  while (v) {
    n++;
    v = v >> 1;
  }
  return n;
}
unsigned _page_size = sysconf(_SC_PAGESIZE);
unsigned long _page_mask = ~(_page_size - 1);
unsigned _page_shift = _get_bits_of(_page_size);

int _num_threads = 0;

// file layouts
struct ceph_file_layout g_OSD_FileLayout = {
 fl_stripe_unit: 1<<22,
 fl_stripe_count: 1,
 fl_object_size: 1<<22,
 fl_object_stripe_unit: 0,
 fl_pg_preferred: -1,
 fl_pg_type: CEPH_PG_TYPE_REP,
 fl_pg_size: 2
};

struct ceph_file_layout g_OSD_MDDirLayout = {
 fl_stripe_unit: 1<<22,
 fl_stripe_count: 1,
 fl_object_size: 1<<22,
 fl_object_stripe_unit: 0,
 fl_pg_preferred: -1,
 fl_pg_type: CEPH_PG_TYPE_REP,
 fl_pg_size: 2
};

struct ceph_file_layout g_OSD_MDLogLayout = {
 fl_stripe_unit: 1<<20,
 fl_stripe_count: 1,
 fl_object_size: 1<<20,
 fl_object_stripe_unit: 0,
 fl_pg_preferred: -1,
 fl_pg_type: CEPH_PG_TYPE_REP,
 fl_pg_size: 2
};

struct ceph_file_layout g_OSD_MDAnchorTableLayout = {
 fl_stripe_unit: 1<<20,
 fl_stripe_count: 1,
 fl_object_size: 1<<20,
 fl_object_stripe_unit: 0,
 fl_pg_preferred: -1,
 fl_pg_type: CEPH_PG_TYPE_REP,
 fl_pg_size: 2
};

#include <msg/msg_types.h>

// fake osd failures: osd -> time
std::map<entity_name_t,float> g_fake_kill_after;
std::map<int,float> g_fake_osd_down;
std::map<int,float> g_fake_osd_out;

entity_addr_t g_my_addr;

md_config_t g_debug_after_conf;

md_config_t g_conf = {
  num_mon: 1,
  num_mds: 1,
  num_osd: 4,
  num_client: 1,

  mkfs: false,

  daemonize: false,

  // profiling and debugging
  log: true,
  log_interval: 1,
  log_name: (char*)0,

  log_messages: true,
  log_pins: true,

  logger_calc_variance: true,

  dout_dir: 0, //"out",

  fake_clock: false,
  fakemessenger_serialize: true,

  fake_osdmap_expand: 0,
  fake_osdmap_updates: 0,
  fake_osd_mttf: 0,
  fake_osd_mttr: 0,

  osd_remount_at: 0,

  kill_after: 0,

  tick: 0,

  debug: 0,
  debug_mds: 1,
  debug_mds_balancer: 1,
  debug_mds_log: 1,
  debug_mds_log_expire: 1,
  debug_mds_migrator: 1,
  debug_buffer: 0,
  debug_timer: 0,
  debug_filer: 0,
  debug_objecter: 0,
  debug_journaler: 0,
  debug_objectcacher: 0,
  debug_client: 0,
  debug_osd: 0,
  debug_ebofs: 1,
  debug_bdev: 1,         // block device
  debug_ns: 0,
  debug_ms: 0,
  debug_mon: 1,
  debug_paxos: 0,
  
  debug_after: 0,
  
  // -- misc --
  use_abspaths: false,      // make monitorstore et al use absolute path (to workaround FUSE chdir("/"))

  // --- clock ---
  clock_lock: false,
  clock_tare: false,
  
  // --- messenger ---
  ms_hosts: ".ceph_hosts",
  ms_tcp_nodelay: true,
  ms_retry_interval: 2.0,  // how often to attempt reconnect 
  ms_fail_interval: 15.0,  // fail after this long
  ms_die_on_failure: false,

  ms_stripe_osds: false,
  ms_skip_rank0: false,
  ms_overlay_clients: false,


  // --- mon ---
  mon_tick_interval: 5,
  mon_osd_down_out_interval: 5,  // seconds
  mon_lease: 5,  // seconds    // lease interval
  mon_lease_renew_interval: 3, // on leader, to renew the lease
  mon_lease_ack_timeout: 10.0, // on leader, if lease isn't acked by all peons
  mon_lease_timeout: 10.0,     // on peon, if lease isn't extended
  mon_accept_timeout: 10.0,    // on leader, if paxos update isn't accepted
  mon_stop_on_last_unmount: false,
  mon_stop_with_last_mds: false,
  mon_allow_mds_bully: false,   // allow a booting mds to (forcibly) claim an mds # .. FIXME

  paxos_propose_interval: 1.0,  // gather updates for this long before proposing a map update

  // --- client ---
  client_cache_size: 1000,
  client_cache_mid: .5,
  client_cache_stat_ttl: 0, // seconds until cached stat results become invalid
  client_cache_readdir_ttl: 1,  // 1 second only
  client_use_random_mds:  false,
  client_mount_timeout: 10.0,  // retry every N seconds
  client_tick_interval: 1.0,
  client_hack_balance_reads: false,
  client_trace: 0,
  fuse_direct_io: 0,
  fuse_ll: true,
  
  // --- objectcacher ---
  client_oc: true,
  client_oc_size:      1024*1024* 10,    // MB * n
  client_oc_max_dirty: 1024*1024* 10,    // MB * n  (dirty OR tx)
  client_oc_max_sync_write: 128*1024,   // synx writes >= this use wrlock

  // --- objecter ---
  objecter_buffer_uncommitted: true,  // this must be true for proper failure handling
  objecter_map_request_interval: 15.0, // request a new map every N seconds, if we have pending io
  objecter_tick_interval: 5.0,
  objecter_timeout: 10.0,    // before we ask for a map

  // --- journaler ---
  journaler_allow_split_entries: true,
  journaler_safe: false,  // wait for COMMIT on journal writes
  journaler_write_head_interval: 15,
  journaler_cache: false, // cache writes for later readback
  journaler_prefetch_periods: 50,   // * journal object size (1~MB? see above)
  journaler_batch_interval: .001,   // seconds.. max add'l latency we artificially incur
  journaler_batch_max: 16384,        // max bytes we'll delay flushing

  // --- mds ---
  mds_cache_size: 300000,  //MDS_CACHE_SIZE,
  mds_cache_mid: .7,

  mds_decay_halflife: 5,

  mds_beacon_interval: 4, //30.0,
  mds_beacon_grace: 15, //60*60.0,

  mds_cap_timeout: 100,        // cap bits time out if client idle
  mds_session_autoclose: 300, // autoclose idle session 

  mds_tick_interval: 5,

  mds_log: true,
  mds_log_max_events: -1, //MDS_CACHE_SIZE / 3,
  mds_log_max_segments: 100,
  mds_log_max_expiring: 20,
  mds_log_pad_entry: 128,//256,//64,
  mds_log_eopen_size: 100,   // # open inodes per log entry

  mds_bal_sample_interval: 3.0,  // every 5 seconds
  mds_bal_replicate_threshold: 8000,
  mds_bal_unreplicate_threshold: 0,//500,
  mds_bal_split_size: 10000,
  mds_bal_split_rd: 25000,
  mds_bal_split_wr: 10000,
  mds_bal_merge_size: 50,
  mds_bal_merge_rd: 1000,
  mds_bal_merge_wr: 1000,
  mds_bal_interval: 10,           // seconds
  mds_bal_fragment_interval: 2,      // seconds
  mds_bal_idle_threshold: 0, //.1,
  mds_bal_max: -1,
  mds_bal_max_until: -1,

  mds_bal_mode: 0,
  mds_bal_min_rebalance: .1,  // must be this much above average before we export anything
  mds_bal_min_start: .2,      // if we need less than this, we don't do anything
  mds_bal_need_min: .8,       // take within this range of what we need
  mds_bal_need_max: 1.2,
  mds_bal_midchunk: .3,       // any sub bigger than this taken in full
  mds_bal_minchunk: .001,     // never take anything smaller than this

  mds_trim_on_rejoin: true,
  mds_shutdown_check: 0, //30,

  mds_verify_export_dirauth: true,

  mds_local_osd: false,

  mds_thrash_exports: 0,
  mds_thrash_fragments: 0,
  mds_dump_cache_on_map: false,
  mds_dump_cache_after_rejoin: true,

  mds_hack_log_expire_for_better_stats: false,

  // --- osd ---
  osd_rep: OSD_REP_PRIMARY,

  osd_balance_reads: false,  // send from client to replica
  osd_flash_crowd_iat_threshold: 0,//100,
  osd_flash_crowd_iat_alpha: 0.125,
  osd_balance_reads_temp: 100,
  
  osd_shed_reads: false,     // forward from primary to replica
  osd_shed_reads_min_latency: .01,       // min local latency
  osd_shed_reads_min_latency_diff: .01,  // min latency difference
  osd_shed_reads_min_latency_ratio: 1.5,  // 1.2 == 20% higher than peer

  osd_immediate_read_from_cache: false,//true, // osds to read from the cache immediately?
  osd_exclusive_caching: true,         // replicas evict replicated writes

  osd_stat_refresh_interval: .5,

  osd_pg_bits: 4,  // bits per osd
  osd_object_layout: CEPH_OBJECT_LAYOUT_HASHINO,//LINEAR,//HASHINO,
  osd_pg_layout: CEPH_PG_LAYOUT_CRUSH,//LINEAR,//CRUSH,
  osd_max_rep: 4,
  osd_min_raid_width: 4,
  osd_max_raid_width: 3, //6, 

  osd_maxthreads: 2,    // 0 == no threading
  osd_max_opq: 10,
  osd_mkfs: false,
  osd_age: .8,
  osd_age_time: 0,
  osd_heartbeat_interval: 1,
  osd_heartbeat_grace: 30,
  osd_pg_stats_interval:  5,
  osd_replay_window: 5,
  osd_max_pull: 2,
  osd_pad_pg_log: false,

  osd_auto_weight: false,

  osd_hack_fast_startup: false,  // this breaks localized pgs.

  
  // --- fakestore ---
  fakestore_fake_sync: .2,    // seconds
  fakestore_fsync: false,//true,
  fakestore_writesync: false,
  fakestore_syncthreads: 4,
  fakestore_fake_attrs: false,
  fakestore_fake_collections: false,   
  fakestore_dev: 0,

  // --- ebofs ---
  ebofs: 1,
  ebofs_cloneable: false,
  ebofs_verify: false,
  ebofs_commit_ms:      1000,       // 0 = no forced commit timeout (for debugging/tracing)
  ebofs_oc_size:        10000,      // onode cache
  ebofs_cc_size:        10000,      // cnode cache
  ebofs_bc_size:        (50 *256), // 4k blocks, *256 for MB
  ebofs_bc_max_dirty:   (30 *256), // before write() will block
  ebofs_max_prefetch: 1000, // 4k blocks
  ebofs_realloc: false,    // hrm, this can cause bad fragmentation, don't use!
  ebofs_verify_csum_on_read: true,
  ebofs_journal_dio: false,
  ebofs_journal_max_write_bytes: 0,
  ebofs_journal_max_write_entries: 10,

  // --- block device ---
  bdev_lock: true,
  bdev_iothreads:   1,         // number of ios to queue with kernel
  bdev_idle_kick_after_ms: 100,  // ms
  bdev_el_fw_max_ms: 10000,      // restart elevator at least once every 1000 ms
  bdev_el_bw_max_ms: 3000,       // restart elevator at least once every 300 ms
  bdev_el_bidir: false,          // bidirectional elevator?
  bdev_iov_max: 512,            // max # iov's to collect into a single readv()/writev() call
  bdev_debug_check_io_overlap: true,   // [DEBUG] check for any pending io overlaps
  bdev_fake_mb: 0,
  bdev_fake_max_mb:  0

#ifdef USE_OSBDB
  ,
  bdbstore: false,
  debug_bdbstore: 1,
  bdbstore_btree: false,
  bdbstore_ffactor: 0,
  bdbstore_nelem: 0,
  bdbstore_pagesize: 0,
  bdbstore_cachesize: 0,
  bdbstore_transactional: false
#endif // USE_OSBDB
};


#include <stdlib.h>
#include <string.h>


void env_to_vec(std::vector<const char*>& args) 
{
  const char *p = getenv("CEPH_ARGS");
  if (!p) return;
  
  static char buf[1000];  
  int len = strlen(p);
  memcpy(buf, p, len);
  buf[len] = 0;
  //cout << "CEPH_ARGS " << buf << endl;

  int l = 0;
  for (int i=0; i<len; i++) {
    if (buf[i] == ' ') {
      buf[i] = 0;
      args.push_back(buf+l);
      //cout << "arg " << (buf+l) << endl;
      l = i+1;
    }
  }
  args.push_back(buf+l);
  //cout << "arg " << (buf+l) << endl;
}


void argv_to_vec(int argc, const char **argv,
                 std::vector<const char*>& args)
{
  for (int i=1; i<argc; i++)
    args.push_back(argv[i]);
}

void vec_to_argv(std::vector<const char*>& args,
                 int& argc, const char **&argv)
{
  argv = (const char**)malloc(sizeof(char*) * argc);
  argc = 1;
  argv[0] = "asdf";

  for (unsigned i=0; i<args.size(); i++) 
    argv[argc++] = args[i];
}

bool parse_ip_port(const char *s, entity_addr_t& a)
{
  int count = 0; // digit count
  int off = 0;

  while (1) {
    // parse the #.
    int val = 0;
    int numdigits = 0;
    
    while (*s >= '0' && *s <= '9') {
      int digit = *s - '0';
      //cout << "digit " << digit << endl;
      val *= 10;
      val += digit;
      numdigits++;
      s++; off++;
    }

    if (numdigits == 0) {
      cerr << "no digits at off " << off << std::endl;
      return false;           // no digits
    }
    if (count < 3 && *s != '.') {
      cerr << "should period at " << off << std::endl;
      return false;   // should have 3 periods
    }
    s++; off++;

    if (count <= 3)
      a.set_ipquad(count, val);
    else 
      a.set_port(val);
    
    count++;
    if (count == 4 && *(s-1) != ':') break;
    if (count == 5) break;  
  }
  
  return true;
}



void parse_config_options(std::vector<const char*>& args)
{
  std::vector<const char*> nargs;

  for (unsigned i=0; i<args.size(); i++) {
    if (strcmp(args[i],"--bind") == 0) 
      assert(parse_ip_port(args[++i], g_my_addr));
    else if (strcmp(args[i], "--nummon") == 0) 
      g_conf.num_mon = atoi(args[++i]);
    else if (strcmp(args[i], "--nummds") == 0) 
      g_conf.num_mds = atoi(args[++i]);
    else if (strcmp(args[i], "--numclient") == 0) 
      g_conf.num_client = atoi(args[++i]);
    else if (strcmp(args[i], "--numosd") == 0) 
      g_conf.num_osd = atoi(args[++i]);
    
    else if (strcmp(args[i], "--daemonize") == 0 ||
	     strcmp(args[i], "-d") == 0)
      g_conf.daemonize = true;	     

    else if (strcmp(args[i], "--ms_hosts") == 0)
      g_conf.ms_hosts = args[++i];
    else if (strcmp(args[i], "--ms_stripe_osds") == 0)
      g_conf.ms_stripe_osds = true;
    else if (strcmp(args[i], "--ms_skip_rank0") == 0)
      g_conf.ms_skip_rank0 = true;
    else if (strcmp(args[i], "--ms_overlay_clients") == 0)
      g_conf.ms_overlay_clients = true;
    else if (strcmp(args[i], "--ms_die_on_failure") == 0)
      g_conf.ms_die_on_failure = true;

    /*else if (strcmp(args[i], "--tcp_log") == 0)
      g_conf.tcp_log = true;
    else if (strcmp(args[i], "--tcp_multi_out") == 0)
      g_conf.tcp_multi_out = atoi(args[++i]);
    */

    else if (strcmp(args[i], "--mkfs") == 0) 
      g_conf.osd_mkfs = g_conf.mkfs = 1; //atoi(args[++i]);

    else if (strcmp(args[i], "--fake_osdmap_expand") == 0) 
      g_conf.fake_osdmap_expand = atoi(args[++i]);
    else if (strcmp(args[i], "--fake_osdmap_updates") == 0) 
      g_conf.fake_osdmap_updates = atoi(args[++i]);
    else if (strcmp(args[i], "--fake_osd_mttf") == 0) 
      g_conf.fake_osd_mttf = atoi(args[++i]);
    else if (strcmp(args[i], "--fake_osd_mttr") == 0) 
      g_conf.fake_osd_mttr = atoi(args[++i]);

    else if (strcmp(args[i], "--fake_kill_osd_after") == 0) {
      g_fake_kill_after[entity_name_t(entity_name_t::TYPE_OSD, atoi(args[i+1]))] = atof(args[i+2]); 
      i += 2;
    }
    else if (strcmp(args[i], "--fake_kill_mds_after") == 0) {
      g_fake_kill_after[entity_name_t(entity_name_t::TYPE_MDS, atoi(args[i+1]))] = atof(args[i+2]);
      i += 2;
    }
    else if (strcmp(args[i], "--fake_kill_mon_after") == 0) {
      g_fake_kill_after[entity_name_t(entity_name_t::TYPE_MON, atoi(args[i+1]))] = atof(args[i+2]);
      i += 2;
    }
    else if (strcmp(args[i], "--fake_kill_client_after") == 0) {
      g_fake_kill_after[entity_name_t(entity_name_t::TYPE_CLIENT, atoi(args[i+1]))] = atof(args[i+2]);
      i += 2;
    }

    else if (strcmp(args[i], "--fake_osd_down") == 0) {
      int osd = atoi(args[++i]);
      float when = atof(args[++i]);
      g_fake_osd_down[osd] = when;
    }
    else if (strcmp(args[i], "--fake_osd_out") == 0) {
      int osd = atoi(args[++i]);
      float when = atof(args[++i]);
      g_fake_osd_out[osd] = when;
    }
    else if (strcmp(args[i], "--osd_remount_at") == 0) 
      g_conf.osd_remount_at = atoi(args[++i]);
    //else if (strcmp(args[i], "--fake_osd_sync") == 0) 
    //g_conf.fake_osd_sync = atoi(args[++i]);

    
    
    else if (strcmp(args[i], "--doutdir") == 0) {
      g_conf.dout_dir = args[++i];
    }

    else if (strcmp(args[i], "--debug") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug = atoi(args[++i]);
      else 
        g_debug_after_conf.debug = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds_balancer") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds_balancer = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds_balancer = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds_log") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds_log = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds_log = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds_log_expire") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds_log_expire = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds_log_expire = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mds_migrator") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mds_migrator = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mds_migrator = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_buffer") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_buffer = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_buffer = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_timer") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_timer = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_timer = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_filer") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_filer = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_filer = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_objecter") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_objecter = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_objecter = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_journaler") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_journaler = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_journaler = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_objectcacher") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_objectcacher = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_objectcacher = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_client") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_client = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_client = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_osd") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_osd = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_osd = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_ebofs") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_ebofs = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_ebofs = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_bdev") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_bdev = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_bdev = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_ms") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_ms = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_ms = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_mon") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_mon = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_mon = atoi(args[++i]);
    else if (strcmp(args[i], "--debug_paxos") == 0) 
      if (!g_conf.debug_after) 
        g_conf.debug_paxos = atoi(args[++i]);
      else 
        g_debug_after_conf.debug_paxos = atoi(args[++i]);

    else if (strcmp(args[i], "--debug_after") == 0) {
      g_conf.debug_after = atoi(args[++i]);
      g_debug_after_conf = g_conf;
    }

    else if (strcmp(args[i], "--log") == 0) 
      g_conf.log = atoi(args[++i]);
    else if (strcmp(args[i], "--log_name") == 0) 
      g_conf.log_name = args[++i];

    else if (strcmp(args[i], "--fakemessenger_serialize") == 0) 
      g_conf.fakemessenger_serialize = atoi(args[++i]);


    else if (strcmp(args[i], "--clock_lock") == 0) 
      g_conf.clock_lock = atoi(args[++i]);
    else if (strcmp(args[i], "--clock_tare") == 0) 
      g_conf.clock_tare = atoi(args[++i]);

    else if (strcmp(args[i], "--objecter_buffer_uncommitted") == 0) 
      g_conf.objecter_buffer_uncommitted = atoi(args[++i]);

    else if (strcmp(args[i], "--journaler_safe") == 0) 
      g_conf.journaler_safe = atoi(args[++i]);
    else if (strcmp(args[i], "--journaler_cache") == 0) 
      g_conf.journaler_cache = atoi(args[++i]);
    else if (strcmp(args[i], "--journaler_batch_interval") == 0) 
      g_conf.journaler_batch_interval = atof(args[++i]);
    else if (strcmp(args[i], "--journaler_batch_max") == 0) 
      g_conf.journaler_batch_max = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_cache_size") == 0) 
      g_conf.mds_cache_size = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_beacon_interval") == 0) 
      g_conf.mds_beacon_interval = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_beacon_grace") == 0) 
      g_conf.mds_beacon_grace = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_log") == 0) 
      g_conf.mds_log = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_log_max_events") == 0) 
      g_conf.mds_log_max_events = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_log_max_segments") == 0) 
      g_conf.mds_log_max_segments = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_log_max_expiring") == 0) 
      g_conf.mds_log_max_expiring = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_shutdown_check") == 0) 
      g_conf.mds_shutdown_check = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_decay_halflife") == 0) 
      g_conf.mds_decay_halflife = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_bal_interval") == 0) 
      g_conf.mds_bal_interval = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_rep") == 0) 
      g_conf.mds_bal_replicate_threshold = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_unrep") == 0) 
      g_conf.mds_bal_unreplicate_threshold = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_max") == 0) 
      g_conf.mds_bal_max = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_max_until") == 0) 
      g_conf.mds_bal_max_until = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_bal_split_size") == 0) 
      g_conf.mds_bal_split_size = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_split_rd") == 0) 
      g_conf.mds_bal_split_rd = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_split_wr") == 0) 
      g_conf.mds_bal_split_wr = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_merge_size") == 0) 
      g_conf.mds_bal_merge_size = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_merge_rd") == 0) 
      g_conf.mds_bal_merge_rd = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_merge_wr") == 0) 
      g_conf.mds_bal_merge_wr = atoi(args[++i]);

    else if (strcmp(args[i], "--mds_bal_mode") == 0) 
      g_conf.mds_bal_mode = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_min_start") == 0) 
      g_conf.mds_bal_min_start = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_need_min") == 0) 
      g_conf.mds_bal_need_min = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_need_max") == 0) 
      g_conf.mds_bal_need_max = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_midchunk") == 0) 
      g_conf.mds_bal_midchunk = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_bal_minchunk") == 0) 
      g_conf.mds_bal_minchunk = atoi(args[++i]);
    
    else if (strcmp(args[i], "--mds_local_osd") == 0) 
      g_conf.mds_local_osd = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_thrash_exports") == 0) 
      g_conf.mds_thrash_exports = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_thrash_fragments") == 0) 
      g_conf.mds_thrash_fragments = atoi(args[++i]);
    else if (strcmp(args[i], "--mds_dump_cache_on_map") == 0) 
      g_conf.mds_dump_cache_on_map = true;

    else if (strcmp(args[i], "--mds_hack_log_expire_for_better_stats") == 0) 
      g_conf.mds_hack_log_expire_for_better_stats = atoi(args[++i]);
    
    else if (strcmp(args[i], "--client_use_random_mds") == 0)
      g_conf.client_use_random_mds = true;
    else if (strcmp(args[i], "--client_cache_size") == 0)
      g_conf.client_cache_size = atoi(args[++i]);
    else if (strcmp(args[i], "--client_cache_stat_ttl") == 0)
      g_conf.client_cache_stat_ttl = atoi(args[++i]);
    else if (strcmp(args[i], "--client_cache_readdir_ttl") == 0)
      g_conf.client_cache_readdir_ttl = atoi(args[++i]);
    else if (strcmp(args[i], "--client_trace") == 0)
      g_conf.client_trace = args[++i];

    else if (strcmp(args[i], "--fuse_direct_io") == 0)
      g_conf.fuse_direct_io = atoi(args[++i]);
    else if (strcmp(args[i], "--fuse_ll") == 0)
      g_conf.fuse_ll = atoi(args[++i]);

    else if (strcmp(args[i], "--mon_osd_down_out_interval") == 0)
      g_conf.mon_osd_down_out_interval = atoi(args[++i]);
    else if (strcmp(args[i], "--mon_stop_on_last_unmount") == 0) 
      g_conf.mon_stop_on_last_unmount = atoi(args[++i]);
    else if (strcmp(args[i], "--mon_stop_with_last_mds") == 0)
      g_conf.mon_stop_with_last_mds = atoi(args[++i]);

    else if (strcmp(args[i], "--client_oc") == 0)
      g_conf.client_oc = atoi(args[++i]);
    else if (strcmp(args[i], "--client_oc_size") == 0)
      g_conf.client_oc_size = atoi(args[++i]);
    else if (strcmp(args[i], "--client_oc_max_dirty") == 0)
      g_conf.client_oc_max_dirty = atoi(args[++i]);

    else if (strcmp(args[i], "--client_hack_balance_reads") == 0)
      g_conf.client_hack_balance_reads = atoi(args[++i]);

    else if (strcmp(args[i], "--ebofs") == 0) 
      g_conf.ebofs = 1;
    else if (strcmp(args[i], "--ebofs_cloneable") == 0)
      g_conf.ebofs_cloneable = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_verify") == 0)
      g_conf.ebofs_verify = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_commit_ms") == 0)
      g_conf.ebofs_commit_ms = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_oc_size") == 0)
      g_conf.ebofs_oc_size = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_cc_size") == 0)
      g_conf.ebofs_cc_size = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_bc_size") == 0)
      g_conf.ebofs_bc_size = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_bc_max_dirty") == 0)
      g_conf.ebofs_bc_max_dirty = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_verify_csum_on_read") == 0)
      g_conf.ebofs_verify_csum_on_read = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_max_prefetch") == 0)
      g_conf.ebofs_max_prefetch = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_realloc") == 0)
      g_conf.ebofs_realloc = atoi(args[++i]);
    else if (strcmp(args[i], "--ebofs_journal_dio") == 0)
      g_conf.ebofs_journal_dio = atoi(args[++i]);      
    else if (strcmp(args[i], "--ebofs_journal_max_write_entries") == 0)
      g_conf.ebofs_journal_max_write_entries = atoi(args[++i]);      
    else if (strcmp(args[i], "--ebofs_journal_max_write_bytes") == 0)
      g_conf.ebofs_journal_max_write_bytes = atoi(args[++i]);      

    else if (strcmp(args[i], "--fakestore") == 0) {
      g_conf.ebofs = 0;
      //g_conf.osd_pg_bits = 5;
      //g_conf.osd_maxthreads = 1;   // fucking hell
    }
    else if (strcmp(args[i], "--fakestore_fsync") == 0) 
      g_conf.fakestore_fsync = atoi(args[++i]);
    else if (strcmp(args[i], "--fakestore_writesync") == 0) 
      g_conf.fakestore_writesync = atoi(args[++i]);
    else if (strcmp(args[i], "--fakestore_dev") == 0) 
      g_conf.fakestore_dev = args[++i];
    else if (strcmp(args[i], "--fakestore_fake_attrs") == 0) 
      g_conf.fakestore_fake_attrs = true;//atoi(args[++i]);
    else if (strcmp(args[i], "--fakestore_fake_collections") == 0) 
      g_conf.fakestore_fake_collections = true;//atoi(args[++i]);

    else if (strcmp(args[i], "--osd_balance_reads") == 0) 
      g_conf.osd_balance_reads = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_flash_crowd_iat_threshold") == 0) 
      g_conf.osd_flash_crowd_iat_threshold = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_flash_crowd_iat_alpha") == 0) 
      g_conf.osd_flash_crowd_iat_alpha = atoi(args[++i]);

    else if (strcmp(args[i], "--osd_shed_reads") == 0) 
      g_conf.osd_shed_reads = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_shed_reads_min_latency") == 0) 
      g_conf.osd_shed_reads_min_latency = atof(args[++i]);
    else if (strcmp(args[i], "--osd_shed_reads_min_latency_diff") == 0) 
      g_conf.osd_shed_reads_min_latency_diff = atof(args[++i]);
    else if (strcmp(args[i], "--osd_shed_reads_min_latency_ratio") == 0) 
      g_conf.osd_shed_reads_min_latency_ratio = atof(args[++i]);

    else if ( strcmp(args[i],"--osd_immediate_read_from_cache" ) == 0)
      g_conf.osd_immediate_read_from_cache = atoi(args[++i]);
    else if ( strcmp(args[i],"--osd_exclusive_caching" ) == 0)
      g_conf.osd_exclusive_caching = atoi(args[++i]);

    else if ( strcmp(args[i],"--osd_stat_refresh_interval" ) == 0)
      g_conf.osd_stat_refresh_interval = atof(args[++i]);

    else if (strcmp(args[i], "--osd_rep") == 0) 
      g_conf.osd_rep = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_rep_chain") == 0) 
      g_conf.osd_rep = OSD_REP_CHAIN;
    else if (strcmp(args[i], "--osd_rep_splay") == 0) 
      g_conf.osd_rep = OSD_REP_SPLAY;
    else if (strcmp(args[i], "--osd_rep_primary") == 0) 
      g_conf.osd_rep = OSD_REP_PRIMARY;
    else if (strcmp(args[i], "--osd_mkfs") == 0) 
      g_conf.osd_mkfs = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_heartbeat_interval") == 0) 
      g_conf.osd_heartbeat_interval = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_heartbeat_grace") == 0) 
      g_conf.osd_heartbeat_grace = atoi(args[++i]);
    
    else if (strcmp(args[i], "--osd_age") == 0) 
      g_conf.osd_age = atof(args[++i]);
    else if (strcmp(args[i], "--osd_age_time") == 0) 
      g_conf.osd_age_time = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_pg_bits") == 0) 
      g_conf.osd_pg_bits = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_max_rep") == 0) 
      g_conf.osd_max_rep = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_maxthreads") == 0) 
      g_conf.osd_maxthreads = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_max_pull") == 0) 
      g_conf.osd_max_pull = atoi(args[++i]);
    else if (strcmp(args[i], "--osd_pad_pg_log") == 0) 
      g_conf.osd_pad_pg_log = atoi(args[++i]);

    else if (strcmp(args[i], "--osd_auto_weight") == 0) 
      g_conf.osd_auto_weight = atoi(args[++i]);

    else if (strcmp(args[i], "--osd_hack_fast_startup") == 0) 
      g_conf.osd_hack_fast_startup = atoi(args[++i]);

    else if (strcmp(args[i], "--bdev_lock") == 0) 
      g_conf.bdev_lock = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_el_bidir") == 0) 
      g_conf.bdev_el_bidir = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_iothreads") == 0) 
      g_conf.bdev_iothreads = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_idle_kick_after_ms") == 0) 
      g_conf.bdev_idle_kick_after_ms = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_fake_mb") == 0) 
      g_conf.bdev_fake_mb = atoi(args[++i]);
    else if (strcmp(args[i], "--bdev_fake_max_mb") == 0) 
      g_conf.bdev_fake_max_mb = atoi(args[++i]);

    else if (strcmp(args[i], "--osd_object_layout") == 0) {
      i++;
      if (strcmp(args[i], "linear") == 0) g_conf.osd_object_layout = CEPH_OBJECT_LAYOUT_LINEAR;
      else if (strcmp(args[i], "hashino") == 0) g_conf.osd_object_layout = CEPH_OBJECT_LAYOUT_HASHINO;
      else if (strcmp(args[i], "hash") == 0) g_conf.osd_object_layout = CEPH_OBJECT_LAYOUT_HASH;
      else assert(0);
    }
    
    else if (strcmp(args[i], "--osd_pg_layout") == 0) {
      i++;
      if (strcmp(args[i], "linear") == 0) g_conf.osd_pg_layout = CEPH_PG_LAYOUT_LINEAR;
      else if (strcmp(args[i], "hash") == 0) g_conf.osd_pg_layout = CEPH_PG_LAYOUT_HASH;
      else if (strcmp(args[i], "hybrid") == 0) g_conf.osd_pg_layout = CEPH_PG_LAYOUT_HYBRID;
      else if (strcmp(args[i], "crush") == 0) g_conf.osd_pg_layout = CEPH_PG_LAYOUT_CRUSH;
      else assert(0);
    }
    
    else if (strcmp(args[i], "--kill_after") == 0) 
      g_conf.kill_after = atoi(args[++i]);
    else if (strcmp(args[i], "--tick") == 0) 
      g_conf.tick = atoi(args[++i]);

    else if (strcmp(args[i], "--file_layout_unit") == 0) 
      g_OSD_FileLayout.fl_stripe_unit = atoi(args[++i]);
    else if (strcmp(args[i], "--file_layout_count") == 0) 
      g_OSD_FileLayout.fl_stripe_count = atoi(args[++i]);
    else if (strcmp(args[i], "--file_layout_osize") == 0) 
      g_OSD_FileLayout.fl_object_size = atoi(args[++i]);
    else if (strcmp(args[i], "--file_layout_pg_type") == 0) 
      g_OSD_FileLayout.fl_pg_type = atoi(args[++i]);
    else if (strcmp(args[i], "--file_layout_pg_size") == 0) 
      g_OSD_FileLayout.fl_pg_size = atoi(args[++i]);

    else if (strcmp(args[i], "--meta_dir_layout_unit") == 0) 
      g_OSD_MDDirLayout.fl_stripe_unit = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_dir_layout_scount") == 0) 
      g_OSD_MDDirLayout.fl_stripe_count = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_dir_layout_osize") == 0) 
      g_OSD_MDDirLayout.fl_object_size = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_dir_layout_pg_type") == 0) 
      g_OSD_MDDirLayout.fl_pg_type = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_dir_layout_pg_size") == 0) 
      g_OSD_MDDirLayout.fl_pg_size = atoi(args[++i]);

    else if (strcmp(args[i], "--meta_log_layout_unit") == 0) 
      g_OSD_MDLogLayout.fl_stripe_unit = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_log_layout_scount") == 0) 
      g_OSD_MDLogLayout.fl_stripe_count = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_log_layout_osize") == 0) 
      g_OSD_MDLogLayout.fl_object_size = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_log_layout_pg_type") == 0) 
      g_OSD_MDLogLayout.fl_pg_type = atoi(args[++i]);
    else if (strcmp(args[i], "--meta_log_layout_pg_size") == 0) {
      g_OSD_MDLogLayout.fl_pg_size = atoi(args[++i]);
      if (!g_OSD_MDLogLayout.fl_pg_size)
        g_conf.mds_log = false;
    }

#ifdef USE_OSBDB
    else if (strcmp(args[i], "--bdbstore") == 0) {
      g_conf.bdbstore = true;
      g_conf.ebofs = 0;
    }
    else if (strcmp(args[i], "--bdbstore-btree") == 0) {
      g_conf.bdbstore_btree = true;
    }
    else if (strcmp(args[i], "--bdbstore-hash-ffactor") == 0) {
      g_conf.bdbstore_ffactor = atoi(args[++i]);
    }
    else if (strcmp(args[i], "--bdbstore-hash-nelem") == 0) {
      g_conf.bdbstore_nelem = atoi(args[++i]);
    }
    else if (strcmp(args[i], "--bdbstore-hash-pagesize") == 0) {
      g_conf.bdbstore_pagesize = atoi(args[++i]);
    }
    else if (strcmp(args[i], "--bdbstore-cachesize") == 0) {
      g_conf.bdbstore_cachesize = atoi(args[++i]);
    }
    else if (strcmp(args[i], "--bdbstore-transactional") == 0) {
      g_conf.bdbstore_transactional = true;
    }
    else if (strcmp(args[i], "--debug-bdbstore") == 0) {
      g_conf.debug_bdbstore = atoi(args[++i]);
    }
#endif // USE_OSBDB

    else {
      nargs.push_back(args[i]);
    }
  }

  // redirect dout?
  /*
  if (g_conf.dout_dir) {
    struct stat st;
    int r = ::stat(g_conf.dout_dir, &st);
    if (r != 0)
      g_conf.dout_dir = 0;
  }
  */
  if (g_conf.dout_dir) {
    char fn[80];
    char hostname[80];
    gethostname(hostname, 79);

    if (g_conf.dout_dir[0] == '/') 
      strcpy(_dout_dir, g_conf.dout_dir);
    else {
      getcwd(_dout_dir, 100);
      strcat(_dout_dir, "/");
      strcat(_dout_dir, g_conf.dout_dir);
    }
    sprintf(_dout_file, "%s.%d", hostname, getpid());

    sprintf(fn, "%s/%s", _dout_dir, _dout_file);
    std::ofstream *out = new std::ofstream(fn, ios::trunc|ios::out);
    if (!out->is_open()) {
      std::cerr << "error opening output file " << fn << std::endl;
      delete out;
    } else {
      _dout = out;
    }
  }

  args = nargs;
}

int rename_output_file()  // after calling daemon()
{
  if (g_conf.dout_dir) {
    char oldfn[100];
    char newfn[100];
    char hostname[80];
    gethostname(hostname, 79);
    
    sprintf(oldfn, "%s/%s", _dout_dir, _dout_file);
    sprintf(newfn, "%s/%s.%d", _dout_dir, hostname, getpid());
    ::rename(oldfn, newfn);
    sprintf(_dout_file, "%s.%d", hostname, getpid());

    if (_dout_symlink_path[0]) {
      ::unlink(_dout_symlink_path);
      ::symlink(_dout_file, _dout_symlink_path);
    }
  }
  return 0;
}

int create_courtesy_output_symlink(const char *type, int n)
{
  if (g_conf.dout_dir) {
    sprintf(_dout_symlink_path, "%s/%s%d", _dout_dir, type, n);
    ::unlink(_dout_symlink_path);
    ::symlink(_dout_file, _dout_symlink_path);
  }
  return 0;
}

