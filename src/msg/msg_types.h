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

#ifndef __MSG_TYPES_H
#define __MSG_TYPES_H

#include "include/types.h"
#include "include/blobhash.h"
#include "tcp.h"

class entity_name_t {
public:
  struct ceph_entity_name v;

public:
  static const int TYPE_MON = CEPH_ENTITY_TYPE_MON;
  static const int TYPE_MDS = CEPH_ENTITY_TYPE_MDS;
  static const int TYPE_OSD = CEPH_ENTITY_TYPE_OSD;
  static const int TYPE_CLIENT = CEPH_ENTITY_TYPE_CLIENT;
  static const int TYPE_ADMIN = CEPH_ENTITY_TYPE_ADMIN;

  static const int NEW = -1;

  // cons
  entity_name_t() { v.type = v.num = 0; }
  entity_name_t(int t, int n) { v.type = t; v.num = n; }
  entity_name_t(const ceph_entity_name &n) : v(n) { }

  // static cons
  static entity_name_t MON(int i=NEW) { return entity_name_t(TYPE_MON, i); }
  static entity_name_t MDS(int i=NEW) { return entity_name_t(TYPE_MDS, i); }
  static entity_name_t OSD(int i=NEW) { return entity_name_t(TYPE_OSD, i); }
  static entity_name_t CLIENT(int i=NEW) { return entity_name_t(TYPE_CLIENT, i); }
  static entity_name_t ADMIN(int i=NEW) { return entity_name_t(TYPE_ADMIN, i); }
  
  int num() const { return v.num; }
  int type() const { return v.type; }
  const char *type_str() const {
    switch (type()) {
    case TYPE_MDS: return "mds"; 
    case TYPE_OSD: return "osd"; 
    case TYPE_MON: return "mon"; 
    case TYPE_CLIENT: return "client"; 
    case TYPE_ADMIN: return "admin";
    default: return "unknown";
    }    
  }

  bool is_new() const { return num() < 0; }

  bool is_client() const { return type() == TYPE_CLIENT; }
  bool is_mds() const { return type() == TYPE_MDS; }
  bool is_osd() const { return type() == TYPE_OSD; }
  bool is_mon() const { return type() == TYPE_MON; }
  bool is_admin() const { return type() == TYPE_ADMIN; }
};

inline bool operator== (const entity_name_t& l, const entity_name_t& r) { 
  return (l.type() == r.type()) && (l.num() == r.num()); }
inline bool operator!= (const entity_name_t& l, const entity_name_t& r) { 
  return (l.type() != r.type()) || (l.num() != r.num()); }
inline bool operator< (const entity_name_t& l, const entity_name_t& r) { 
  return (l.type() < r.type()) || (l.type() == r.type() && l.num() < r.num()); }

inline std::ostream& operator<<(std::ostream& out, const entity_name_t& addr) {
  //if (addr.is_namer()) return out << "namer";
  if (addr.is_new() || addr.num() < 0)
    return out << addr.type_str() << "?";
  else
    return out << addr.type_str() << addr.num();
}
inline std::ostream& operator<<(std::ostream& out, const ceph_entity_name& addr) {
  return out << *(const entity_name_t*)&addr;
}

namespace __gnu_cxx {
  template<> struct hash< entity_name_t >
  {
    size_t operator()( const entity_name_t m ) const
    {
      return rjhash32(m.v.type) ^ rjhash32(m.v.num);
    }
  };
}



/*
 * an entity's network address.
 * includes a random value that prevents it from being reused.
 * thus identifies a particular process instance.
 * ipv4 for now.
 */
struct entity_addr_t {
  struct ceph_entity_addr v;

  entity_addr_t() { 
    memset(&v, 0, sizeof(v));
    v.ipaddr.sin_family = AF_INET;
  }

  void set_ipquad(int pos, int val) {
    unsigned char *ipq = (unsigned char*)&v.ipaddr.sin_addr.s_addr;
    ipq[pos] = val;
  }
  void set_port(int port) {
    v.ipaddr.sin_port = htons(port);
  }
  int get_port() {
    return ntohs(v.ipaddr.sin_port);
  }
};

inline ostream& operator<<(ostream& out, const entity_addr_t &addr)
{
  return out << addr.v.ipaddr << '/' << addr.v.nonce << '/' << addr.v.erank;
}

inline bool operator==(const entity_addr_t& a, const entity_addr_t& b) { return memcmp(&a, &b, sizeof(a)) == 0; }
inline bool operator!=(const entity_addr_t& a, const entity_addr_t& b) { return memcmp(&a, &b, sizeof(a)) != 0; }
inline bool operator<(const entity_addr_t& a, const entity_addr_t& b) { return memcmp(&a, &b, sizeof(a)) < 0; }
inline bool operator<=(const entity_addr_t& a, const entity_addr_t& b) { return memcmp(&a, &b, sizeof(a)) <= 0; }
inline bool operator>(const entity_addr_t& a, const entity_addr_t& b) { return memcmp(&a, &b, sizeof(a)) > 0; }
inline bool operator>=(const entity_addr_t& a, const entity_addr_t& b) { return memcmp(&a, &b, sizeof(a)) >= 0; }

namespace __gnu_cxx {
  template<> struct hash< entity_addr_t >
  {
    size_t operator()( const entity_addr_t& x ) const
    {
      static blobhash H;
      return H((const char*)&x, sizeof(x));
    }
  };
}


/*
 * a particular entity instance
 */
struct entity_inst_t {
  entity_name_t name;
  entity_addr_t addr;
  entity_inst_t() {}
  entity_inst_t(entity_name_t n, const entity_addr_t& a) : name(n), addr(a) {}
  entity_inst_t(const ceph_entity_inst& i) {
    name.v = i.name;
    addr.v = i.addr;
  }
  entity_inst_t(const ceph_entity_name& n, const ceph_entity_addr &a) {
    name.v = n;
    addr.v = a;
  }
};


inline bool operator==(const entity_inst_t& a, const entity_inst_t& b) { return memcmp(&a, &b, sizeof(a)) == 0; }
inline bool operator!=(const entity_inst_t& a, const entity_inst_t& b) { return memcmp(&a, &b, sizeof(a)) != 0; }
inline bool operator<(const entity_inst_t& a, const entity_inst_t& b) { return memcmp(&a, &b, sizeof(a)) < 0; }
inline bool operator<=(const entity_inst_t& a, const entity_inst_t& b) { return memcmp(&a, &b, sizeof(a)) <= 0; }
inline bool operator>(const entity_inst_t& a, const entity_inst_t& b) { return memcmp(&a, &b, sizeof(a)) > 0; }
inline bool operator>=(const entity_inst_t& a, const entity_inst_t& b) { return memcmp(&a, &b, sizeof(a)) >= 0; }

namespace __gnu_cxx {
  template<> struct hash< entity_inst_t >
  {
    size_t operator()( const entity_inst_t& x ) const
    {
      static blobhash H;
      return H((const char*)&x, sizeof(x));
    }
  };
}

inline ostream& operator<<(ostream& out, const entity_inst_t &i)
{
  return out << i.name << " " << i.addr;
}
inline ostream& operator<<(ostream& out, const ceph_entity_inst &i)
{
  return out << *(const entity_inst_t*)&i;
}



#endif
