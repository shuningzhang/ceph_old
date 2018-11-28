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

#ifndef __CONFIG_H
#define __CONFIG_H

extern struct ceph_file_layout g_OSD_FileLayout;
extern struct ceph_file_layout g_OSD_MDDirLayout;
extern struct ceph_file_layout g_OSD_MDLogLayout;
extern struct ceph_file_layout g_OSD_MDAnchorTableLayout;

#include <vector>
#include <map>

#include "common/Mutex.h"

extern std::map<int,float> g_fake_osd_down;
extern std::map<int,float> g_fake_osd_out;

#define OSD_REP_PRIMARY 0
#define OSD_REP_SPLAY   1
#define OSD_REP_CHAIN   2


#include "msg/msg_types.h"

extern entity_addr_t g_my_addr;

struct md_config_t {
  int  num_mon;
  int  num_mds;
  int  num_osd;
  int  num_client;

  bool mkfs;

  bool daemonize;

  // profiling
  bool  log;
  int   log_interval;
  const char *log_name;

  bool log_messages;
  bool log_pins;

  bool logger_calc_variance;

  const char *dout_dir;

  bool fake_clock;
  bool fakemessenger_serialize;

  int fake_osdmap_expand;
  int fake_osdmap_updates;
  int fake_osd_mttf;
  int fake_osd_mttr;

  int osd_remount_at;

  int kill_after;

  int tick;

  int debug;
  int debug_mds;
  int debug_mds_balancer;
  int debug_mds_log;
  int debug_mds_log_expire;
  int debug_mds_migrator;
  int debug_buffer;
  int debug_timer;
  int debug_filer;
  int debug_objecter;
  int debug_journaler;
  int debug_objectcacher;
  int debug_client;
  int debug_osd;
  int debug_ebofs;
  int debug_bdev;
  int debug_ns;
  int debug_ms;
  int debug_mon;
  int debug_paxos;

  int debug_after;

  // misc
  bool use_abspaths;

  // clock
  bool clock_lock;
  bool clock_tare;

  // messenger

  /*bool tcp_skip_rank0;
  bool tcp_overlay_clients;
  bool tcp_log;
  bool tcp_serial_marshall;
  bool tcp_serial_out;
  bool tcp_multi_out;
  bool tcp_multi_dispatch;
  */

  const char *ms_hosts;
  bool ms_tcp_nodelay;
  double ms_retry_interval;
  double ms_fail_interval;
  bool ms_die_on_failure;

  bool ms_stripe_osds;
  bool ms_skip_rank0;
  bool ms_overlay_clients;

  // mon
  int mon_tick_interval;
  int mon_osd_down_out_interval;
  float mon_lease;
  float mon_lease_renew_interval;
  float mon_lease_ack_timeout;
  float mon_lease_timeout;
  float mon_accept_timeout;
  bool mon_stop_on_last_unmount;
  bool mon_stop_with_last_mds;
  bool mon_allow_mds_bully;

  double paxos_propose_interval;

  // client
  int      client_cache_size;
  float    client_cache_mid;
  int      client_cache_stat_ttl;
  int      client_cache_readdir_ttl;
  bool     client_use_random_mds;          // debug flag
  double   client_mount_timeout;
  double   client_tick_interval;
  bool client_hack_balance_reads;
  const char *client_trace;
  int fuse_direct_io;
  bool fuse_ll;

  // objectcacher
  bool     client_oc;
  int      client_oc_size;
  int      client_oc_max_dirty;
  size_t   client_oc_max_sync_write;

  // objecter
  bool  objecter_buffer_uncommitted;
  double objecter_map_request_interval;
  double objecter_tick_interval;
  double objecter_timeout;

  // journaler
  bool  journaler_allow_split_entries;
  bool  journaler_safe;
  int   journaler_write_head_interval;
  bool  journaler_cache;
  int   journaler_prefetch_periods;
  double journaler_batch_interval;
  size_t journaler_batch_max;
  
  // mds
  int   mds_cache_size;
  float mds_cache_mid;
  
  float mds_decay_halflife;

  float mds_beacon_interval;
  float mds_beacon_grace;

  float mds_cap_timeout;
  float mds_session_autoclose;

  float mds_tick_interval;

  bool mds_log;
  int mds_log_max_events;
  int mds_log_max_segments;
  int mds_log_max_expiring;
  int mds_log_pad_entry;
  int mds_log_eopen_size;
  
  float mds_bal_sample_interval;  
  float mds_bal_replicate_threshold;
  float mds_bal_unreplicate_threshold;
  int mds_bal_split_size;
  float mds_bal_split_rd;
  float mds_bal_split_wr;
  int mds_bal_merge_size;
  float mds_bal_merge_rd;
  float mds_bal_merge_wr;
  int   mds_bal_interval;
  int   mds_bal_fragment_interval;
  float mds_bal_idle_threshold;
  int   mds_bal_max;
  int   mds_bal_max_until;

  int   mds_bal_mode;
  float mds_bal_min_rebalance;
  float mds_bal_min_start;
  float mds_bal_need_min;
  float mds_bal_need_max;
  float mds_bal_midchunk;
  float mds_bal_minchunk;

  bool  mds_trim_on_rejoin;
  int   mds_shutdown_check;

  bool  mds_verify_export_dirauth;     // debug flag

  bool  mds_local_osd;

  int mds_thrash_exports;
  int mds_thrash_fragments;
  bool mds_dump_cache_on_map;
  bool mds_dump_cache_after_rejoin;

  bool mds_hack_log_expire_for_better_stats;

  // osd
  int   osd_rep;

  bool osd_balance_reads;
  int osd_flash_crowd_iat_threshold;  // flash crowd interarrival time threshold in ms
  double osd_flash_crowd_iat_alpha;
  double osd_balance_reads_temp;

  int  osd_shed_reads;
  double osd_shed_reads_min_latency;
  double osd_shed_reads_min_latency_diff;
  double osd_shed_reads_min_latency_ratio;

  bool  osd_immediate_read_from_cache;
  bool  osd_exclusive_caching;
  double osd_stat_refresh_interval;

  int   osd_pg_bits;
  int   osd_object_layout;
  int   osd_pg_layout;
  int   osd_max_rep;
  int   osd_min_raid_width;
  int   osd_max_raid_width;
  int   osd_maxthreads;
  int   osd_max_opq;
  bool  osd_mkfs;
  float   osd_age;
  int   osd_age_time;
  int   osd_heartbeat_interval;  
  int   osd_heartbeat_grace;
  int   osd_pg_stats_interval;
  int   osd_replay_window;
  int   osd_max_pull;
  bool  osd_pad_pg_log;

  bool osd_auto_weight;

  bool osd_hack_fast_startup;

  double   fakestore_fake_sync;
  bool  fakestore_fsync;
  bool  fakestore_writesync;
  int   fakestore_syncthreads;   // such crap
  bool  fakestore_fake_attrs;
  bool  fakestore_fake_collections;
  const char  *fakestore_dev;
  
  // ebofs
  int   ebofs;
  bool  ebofs_cloneable;
  bool  ebofs_verify;
  int   ebofs_commit_ms;
  int   ebofs_oc_size;
  int   ebofs_cc_size;
  off_t ebofs_bc_size;
  off_t ebofs_bc_max_dirty;
  unsigned ebofs_max_prefetch;
  bool  ebofs_realloc;
  bool ebofs_verify_csum_on_read;
  bool ebofs_journal_dio;
  bool ebofs_journal_max_write_bytes;
  bool ebofs_journal_max_write_entries;
  
  // block device
  bool  bdev_lock;
  int   bdev_iothreads;
  int   bdev_idle_kick_after_ms;
  int   bdev_el_fw_max_ms;  
  int   bdev_el_bw_max_ms;
  bool  bdev_el_bidir;
  int   bdev_iov_max;
  bool  bdev_debug_check_io_overlap;
  int   bdev_fake_mb;
  int   bdev_fake_max_mb;

#ifdef USE_OSBDB
  bool bdbstore;
  int debug_bdbstore;
  bool bdbstore_btree;
  int bdbstore_ffactor;
  int bdbstore_nelem;
  int bdbstore_pagesize;
  int bdbstore_cachesize;
  bool bdbstore_transactional;
#endif // USE_OSBDB
};

extern md_config_t g_conf;     
extern md_config_t g_debug_after_conf;     


/**
 * command line / environment argument parsing
 */
void env_to_vec(std::vector<const char*>& args);
void argv_to_vec(int argc, const char **argv,
                 std::vector<const char*>& args);
void vec_to_argv(std::vector<const char*>& args,
                 int& argc, const char **&argv);

void parse_config_options(std::vector<const char*>& args);

extern bool parse_ip_port(const char *s, entity_addr_t& addr);

int create_courtesy_output_symlink(const char *type, int n);
int rename_output_file();


/**
 * for cleaner output, bracket each line with
 * dbeginl (in the dout macro) and dendl (in place of endl).
 */
extern Mutex _dout_lock;
struct _dbeginl_t { _dbeginl_t(int) {} };
struct _dendl_t { _dendl_t(int) {} };
static const _dbeginl_t dbeginl = 0;
static const _dendl_t dendl = 0;

// intentionally conflict with endl
class _bad_endl_use_dendl_t { public: _bad_endl_use_dendl_t(int) {} };
static const _bad_endl_use_dendl_t endl = 0;

inline ostream& operator<<(ostream& out, _dbeginl_t) {
  _dout_lock.Lock();
  return out;
}
inline ostream& operator<<(ostream& out, _dendl_t) {
  out << std::endl;
  _dout_lock.Unlock();
  return out;
}
inline ostream& operator<<(ostream& out, _bad_endl_use_dendl_t) {
  assert(0 && "you are using the wrong endl.. use std::endl or dendl");
  return out;
}

// the streams
extern ostream *_dout;
extern ostream *_derr;

// generic macros
#define generic_dout(x) if ((x) <= g_conf.debug) *_dout << dbeginl
#define generic_derr(x) if ((x) <= g_conf.debug) *_derr << dbeginl

#define pdout(x,p) if ((x) <= (p)) *_dout << dbeginl


#endif
