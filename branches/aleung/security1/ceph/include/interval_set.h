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


#ifndef __INTERVAL_SET_H
#define __INTERVAL_SET_H

#include <map>
#include <ostream>
#include <cassert>
using namespace std;

#ifndef MIN
# define MIN(a,b)  ((a)<=(b) ? (a):(b))
#endif
#ifndef MAX
# define MAX(a,b)  ((a)>=(b) ? (a):(b))
#endif


template<typename T>
class interval_set {
 public:
  map<T,T> m;   // map start -> len  

  // helpers
 private:
  typename map<T,T>::const_iterator find_inc(T start) const {
    typename map<T,T>::const_iterator p = m.lower_bound(start);  // p->first >= start
    if (p != m.begin() &&
        (p == m.end() || p->first > start)) {
      p--;   // might overlap?
      if (p->first + p->second <= start)
        p++; // it doesn't.
    }
    return p;
  }
  
  typename map<T,T>::iterator find_inc_m(T start) {
    typename map<T,T>::iterator p = m.lower_bound(start);
    if (p != m.begin() &&
        (p == m.end() || p->first > start)) {
      p--;   // might overlap?
      if (p->first + p->second <= start)
        p++; // it doesn't.
    }
    return p;
  }
  
  typename map<T,T>::const_iterator find_adj(T start) const {
    typename map<T,T>::const_iterator p = m.lower_bound(start);
    if (p != m.begin() &&
        (p == m.end() || p->first > start)) {
      p--;   // might touch?
      if (p->first + p->second < start)
        p++; // it doesn't.
    }
    return p;
  }
  
  typename map<T,T>::iterator find_adj_m(T start) {
    typename map<T,T>::iterator p = m.lower_bound(start);
    if (p != m.begin() &&
        (p == m.end() || p->first > start)) {
      p--;   // might touch?
      if (p->first + p->second < start)
        p++; // it doesn't.
    }
    return p;
  }
  
 public:
  bool operator==(const interval_set& other) const {
    return m == other.m;
  }

  void clear() {
    m.clear();
  }

  bool contains(T i) const {
    typename map<T,T>::const_iterator p = find_inc(i);
    if (p == m.end()) return false;
    if (p->first > i) return false;
    if (p->first+p->second <= i) return false;
    assert(p->first <= i && p->first+p->second > i);
    return true;
  }
  bool contains(T start, T len) const {
    typename map<T,T>::const_iterator p = find_inc(start);
    if (p == m.end()) return false;
    if (p->first > start) return false;
    if (p->first+p->second <= start) return false;
    assert(p->first <= start && p->first+p->second > start);
    if (p->first+p->second < start+len) return false;
    return true;
  }
  bool intersects(T start, T len) const {
    interval_set a;
    a.insert(start, len);
    interval_set i;
    i.intersection_of( *this, a );
    if (i.empty()) return false;
    return true;
  }

  // outer range of set
  bool empty() const {
    return m.empty();
  }
  T start() const {
    assert(!empty());
    typename map<T,T>::const_iterator p = m.begin();
    return p->first;
  }
  T end() const {
    assert(!empty());
    typename map<T,T>::const_iterator p = m.end();
    p--;
    return p->first+p->second;
  }

  // interval start after p (where p not in set)
  bool starts_after(T i) const {
    assert(!contains(i));
    typename map<T,T>::const_iterator p = find_inc(i);
    if (p == m.end()) return false;
    return true;
  }
  T start_after(T i) const {
    assert(!contains(i));
    typename map<T,T>::const_iterator p = find_inc(i);
    return p->first;
  }

  // interval end that contains start
  T end_after(T start) const {
    assert(contains(start));
    typename map<T,T>::const_iterator p = find_inc(start);
    return p->first+p->second;
  }
  
  void insert(T val) {
    insert(val, 1);
  }

  void insert(T start, T len) {
    //cout << "insert " << start << "~" << len << endl;
    assert(len > 0);
    typename map<T,T>::iterator p = find_adj_m(start);
    if (p == m.end()) {
      m[start] = len;                  // new interval
    } else {
      if (p->first < start) {
        
        if (p->first + p->second != start) {
          //cout << "p is " << p->first << "~" << p->second << ", start is " << start << ", len is " << len << endl;
          assert(0);
        }
        
        assert(p->first + p->second == start);
        p->second += len;               // append to end
        
        typename map<T,T>::iterator n = p;
        n++;
        if (n != m.end() && 
            start+len == n->first) {   // combine with next, too!
          p->second += n->second;
          m.erase(n);
        }
      } else {
        if (start+len == p->first) {
          m[start] = len + p->second;  // append to front 
          m.erase(p);
        } else {
          assert(p->first > start+len);
          m[start] = len;              // new interval
        }
      }
    }
  }
  
  void erase(T val) {
    erase(val, 1);
  }

  void erase(T start, T len) {
    typename map<T,T>::iterator p = find_inc_m(start);

    assert(p != m.end());
    assert(p->first <= start);

    T before = start - p->first;
    assert(p->second >= before+len);
    T after = p->second - before - len;
    
    if (before) 
      p->second = before;        // shorten bit before
    else
      m.erase(p);
    if (after)
      m[start+len] = after;
  }


  void subtract(const interval_set &a) {
    for (typename map<T,T>::const_iterator p = a.m.begin();
         p != a.m.end();
         p++)
      erase(p->first, p->second);
  }

  void insert(const interval_set &a) {
    for (typename map<T,T>::const_iterator p = a.m.begin();
         p != a.m.end();
         p++)
      insert(p->first, p->second);
  }


  void intersection_of(const interval_set &a, const interval_set &b) {
    assert(&a != this);
    assert(&b != this);
    clear();

    typename map<T,T>::const_iterator pa = a.m.begin();
    typename map<T,T>::const_iterator pb = b.m.begin();
    
    while (pa != a.m.end() && pb != b.m.end()) {
      // passing?
      if (pa->first + pa->second <= pb->first) 
        { pa++;  continue; }
      if (pb->first + pb->second <= pa->first) 
        { pb++;  continue; }
      T start = MAX(pa->first, pb->first);
      T end = MIN(pa->first+pa->second, pb->first+pb->second);
      assert(end > start);
      insert(start, end-start);
      if (pa->first+pa->second > pb->first+pb->second)
        pb++;
      else
        pa++; 
    }
  }

  void union_of(const interval_set &a, const interval_set &b) {
    assert(&a != this);
    assert(&b != this);
    clear();
    
    //cout << "union_of" << endl;

    // a
    m = a.m;

    // - (a*b)
    interval_set ab;
    ab.intersection_of(a, b);
    subtract(ab);

    // + b
    insert(b);
    return;
  }
  void union_of(const interval_set &b) {
    interval_set a;
    a.m.swap(m);
    union_of(a, b);
  }

  bool subset_of(const interval_set &big) const {
    for (typename map<T,T>::const_iterator i = m.begin();
         i != m.end();
         i++) 
      if (!big.contains(i->first, i->second)) return false;
    return true;
  }  
  
};

template<class T>
inline ostream& operator<<(ostream& out, const interval_set<T> &s) {
  out << "[";
  for (typename map<T,T>::const_iterator i = s.m.begin();
       i != s.m.end();
       i++) {
    if (i != s.m.begin()) out << ",";
    out << i->first << "~" << i->second;
  }
  out << "]";
  return out;
}


#endif
