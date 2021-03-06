// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#include "msg/Messenger.h"
#include "ObjectCacher.h"
#include "Objecter.h"



/*** ObjectCacher::BufferHead ***/


/*** ObjectCacher::Object ***/

#define dout(l)    if (l<=g_conf.debug || l<=g_conf.debug_objectcacher) *_dout << dbeginl << g_clock.now() << " " << oc->objecter->messenger->get_myname() << ".objectcacher.object(" << oid << ") "


ObjectCacher::BufferHead *ObjectCacher::Object::split(BufferHead *left, off_t off)
{
  dout(20) << "split " << *left << " at " << off << dendl;
  
  // split off right
  ObjectCacher::BufferHead *right = new BufferHead(this);
  right->last_write_tid = left->last_write_tid;
  right->set_state(left->get_state());
  
  off_t newleftlen = off - left->start();
  right->set_start(off);
  right->set_length(left->length() - newleftlen);
  
  // shorten left
  oc->bh_stat_sub(left);
  left->set_length(newleftlen);
  oc->bh_stat_add(left);
  
  // add right
  oc->bh_add(this, right);
  
  // split buffers too
  bufferlist bl;
  bl.claim(left->bl);
  if (bl.length()) {
    assert(bl.length() == (left->length() + right->length()));
    right->bl.substr_of(bl, left->length(), right->length());
    left->bl.substr_of(bl, 0, left->length());
  }
  
  // move read waiters
  if (!left->waitfor_read.empty()) {
    map<off_t, list<Context*> >::iterator o, p = left->waitfor_read.end();
    p--;
    while (p != left->waitfor_read.begin()) {
      if (p->first < right->start()) break;      
      dout(0) << "split  moving waiters at byte " << p->first << " to right bh" << dendl;
      right->waitfor_read[p->first].swap( p->second );
      o = p;
      p--;
      left->waitfor_read.erase(o);
    }
  }
  
  dout(20) << "split    left is " << *left << dendl;
  dout(20) << "split   right is " << *right << dendl;
  return right;
}


void ObjectCacher::Object::merge_left(BufferHead *left, BufferHead *right)
{
  assert(left->end() == right->start());
  assert(left->get_state() == right->get_state());

  dout(10) << "merge_left " << *left << " + " << *right << dendl;
  oc->bh_remove(this, right);
  oc->bh_stat_sub(left);
  left->set_length( left->length() + right->length());
  oc->bh_stat_add(left);

  // data
  left->bl.claim_append(right->bl);
  
  // version 
  // note: this is sorta busted, but should only be used for dirty buffers
  left->last_write_tid =  MAX( left->last_write_tid, right->last_write_tid );
  left->last_write = MAX( left->last_write, right->last_write );

  // waiters
  for (map<off_t, list<Context*> >::iterator p = right->waitfor_read.begin();
       p != right->waitfor_read.end();
       p++) 
    left->waitfor_read[p->first].splice( left->waitfor_read[p->first].begin(),
                                         p->second );
  
  // hose right
  delete right;

  dout(10) << "merge_left result " << *left << dendl;
}

void ObjectCacher::Object::try_merge_bh(BufferHead *bh)
{
  dout(10) << "try_merge_bh " << *bh << dendl;

  // to the left?
  map<off_t,BufferHead*>::iterator p = data.find(bh->start());
  assert(p->second == bh);
  if (p != data.begin()) {
    p--;
    if (p->second->end() == bh->start() &&
	p->second->get_state() == bh->get_state()) {
      merge_left(p->second, bh);
      bh = p->second;
    } else 
      p++;
  }
  // to the right?
  assert(p->second == bh);
  p++;
  if (p != data.end() &&
      p->second->start() == bh->end() &&
      p->second->get_state() == bh->get_state()) 
    merge_left(bh, p->second);
}


/*
 * map a range of bytes into buffer_heads.
 * - create missing buffer_heads as necessary.
 */
int ObjectCacher::Object::map_read(Objecter::OSDRead *rd,
                                   map<off_t, BufferHead*>& hits,
                                   map<off_t, BufferHead*>& missing,
                                   map<off_t, BufferHead*>& rx)
{
  for (list<ObjectExtent>::iterator ex_it = rd->extents.begin();
       ex_it != rd->extents.end();
       ex_it++) {
    
    if (ex_it->oid != oid) continue;
    
    dout(10) << "map_read " << ex_it->oid 
             << " " << ex_it->start << "~" << ex_it->length << dendl;
    
    map<off_t, BufferHead*>::iterator p = data.lower_bound(ex_it->start);
    // p->first >= start
    
    off_t cur = ex_it->start;
    off_t left = ex_it->length;
    
    if (p != data.begin() && 
        (p == data.end() || p->first > cur)) {
      p--;     // might overlap!
      if (p->first + p->second->length() <= cur) 
        p++;   // doesn't overlap.
    }
    
    while (left > 0) {
      // at end?
      if (p == data.end()) {
        // rest is a miss.
        BufferHead *n = new BufferHead(this);
        n->set_start( cur );
        n->set_length( left );
        oc->bh_add(this, n);
        missing[cur] = n;
        dout(20) << "map_read miss " << left << " left, " << *n << dendl;
        cur += left;
        left -= left;
        assert(left == 0);
        assert(cur == ex_it->start + (off_t)ex_it->length);
        break;  // no more.
      }
      
      if (p->first <= cur) {
        // have it (or part of it)
        BufferHead *e = p->second;
        
        if (e->is_clean() ||
            e->is_dirty() ||
            e->is_tx()) {
          hits[cur] = e;     // readable!
          dout(20) << "map_read hit " << *e << dendl;
        } 
        else if (e->is_rx()) {
          rx[cur] = e;       // missing, not readable.
          dout(20) << "map_read rx " << *e << dendl;
        }
        else assert(0);
        
        off_t lenfromcur = MIN(e->end() - cur, left);
        cur += lenfromcur;
        left -= lenfromcur;
        p++;
        continue;  // more?
        
      } else if (p->first > cur) {
        // gap.. miss
        off_t next = p->first;
        BufferHead *n = new BufferHead(this);
        n->set_start( cur );
        n->set_length( MIN(next - cur, left) );
        oc->bh_add(this,n);
        missing[cur] = n;
        cur += MIN(left, n->length());
        left -= MIN(left, n->length());
        dout(20) << "map_read gap " << *n << dendl;
        continue;    // more?
      }
      else 
        assert(0);
    }
  }
  return(0);
}

/*
 * map a range of extents on an object's buffer cache.
 * - combine any bh's we're writing into one
 * - break up bufferheads that don't fall completely within the range
 * //no! - return a bh that includes the write.  may also include other dirty data to left and/or right.
 */
ObjectCacher::BufferHead *ObjectCacher::Object::map_write(Objecter::OSDWrite *wr)
{
  BufferHead *final = 0;

  for (list<ObjectExtent>::iterator ex_it = wr->extents.begin();
       ex_it != wr->extents.end();
       ex_it++) {
    
    if (ex_it->oid != oid) continue;
    
    dout(10) << "map_write oex " << ex_it->oid
             << " " << ex_it->start << "~" << ex_it->length << dendl;
    
    map<off_t, BufferHead*>::iterator p = data.lower_bound(ex_it->start);
    // p->first >= start
    
    off_t cur = ex_it->start;
    off_t left = ex_it->length;
    
    if (p != data.begin() && 
        (p == data.end() || p->first > cur)) {
      p--;     // might overlap or butt up!

      /*// dirty and butts up?
      if (p->first + p->second->length() == cur &&
          p->second->is_dirty()) {
        dout(10) << "map_write will append to tail of " << *p->second << dendl;
        final = p->second;
      }
      */
      if (p->first + p->second->length() <= cur) 
        p++;   // doesn't overlap.
    }    
    
    while (left > 0) {
      off_t max = left;

      // at end ?
      if (p == data.end()) {
        if (final == NULL) {
          final = new BufferHead(this);
          final->set_start( cur );
          final->set_length( max );
          oc->bh_add(this, final);
          dout(10) << "map_write adding trailing bh " << *final << dendl;
        } else {
          final->set_length( final->length() + max );
        }
        left -= max;
        cur += max;
        continue;
      }
      
      dout(10) << "p is " << *p->second << dendl;

      if (p->first <= cur) {
        BufferHead *bh = p->second;
        dout(10) << "map_write bh " << *bh << " intersected" << dendl;
        
        if (p->first < cur) {
          assert(final == 0);
          if (cur + max >= p->first + p->second->length()) {
            // we want right bit (one splice)
            final = split(bh, cur);   // just split it, take right half.
            p++;
            assert(p->second == final);
          } else {
            // we want middle bit (two splices)
            final = split(bh, cur);
            p++;
            assert(p->second == final);
            split(final, cur+max);
          }
        } else if (p->first == cur) {
          if (p->second->length() <= max) {
            // whole bufferhead, piece of cake.
          } else {
            // we want left bit (one splice)
            split(bh, cur + max);        // just split
          }
          if (final) 
            merge_left(final, bh);
          else
            final = bh;
        }
        
        // keep going.
        off_t lenfromcur = final->end() - cur;
        cur += lenfromcur;
        left -= lenfromcur;
        p++;
        continue; 
      } else {
        // gap!
        off_t next = p->first;
        off_t glen = MIN(next - cur, max);
        dout(10) << "map_write gap " << cur << "~" << glen << dendl;
        if (final) {
          final->set_length( final->length() + glen );
        } else {
          final = new BufferHead(this);
          final->set_start( cur );
          final->set_length( glen );
          oc->bh_add(this, final);
        }
        
        cur += glen;
        left -= glen;
        continue;    // more?
      }
    }
  }
  
  // set versoin
  assert(final);
  dout(10) << "map_write final is " << *final << dendl;

  return final;
}


void ObjectCacher::Object::truncate(off_t s)
{
  dout(10) << "truncate to " << s << dendl;
  
  while (!data.empty()) {
	BufferHead *bh = data.rbegin()->second;
	if (bh->end() <= s) 
	  break;
	
	// split bh at truncation point?
	if (bh->start() < s) {
	  split(bh, s);
	  continue;
	}

	// remove bh entirely
	assert(bh->start() >= s);
	oc->bh_remove(this, bh);
	delete bh;
  }
}





/*** ObjectCacher ***/

#undef dout
#define dout(l)    if (l<=g_conf.debug || l<=g_conf.debug_objectcacher) *_dout << dbeginl << g_clock.now() << " " << objecter->messenger->get_myname() << ".objectcacher "



/* private */

void ObjectCacher::close_object(Object *ob) 
{
  dout(10) << "close_object " << *ob << dendl;
  assert(ob->can_close());
  
  // ok!
  objects.erase(ob->get_oid());
  objects_by_ino[ob->get_ino()].erase(ob);
  if (objects_by_ino[ob->get_ino()].empty())
	objects_by_ino.erase(ob->get_ino());
  delete ob;
}




void ObjectCacher::bh_read(BufferHead *bh)
{
  dout(7) << "bh_read on " << *bh << dendl;

  mark_rx(bh);

  // finisher
  C_ReadFinish *onfinish = new C_ReadFinish(this, bh->ob->get_oid(), bh->start(), bh->length());

  // go
  objecter->read(bh->ob->get_oid(), bh->start(), bh->length(), bh->ob->get_layout(), &onfinish->bl,
                 onfinish);
}

void ObjectCacher::bh_read_finish(object_t oid, off_t start, size_t length, bufferlist &bl)
{
  //lock.Lock();
  dout(7) << "bh_read_finish " 
          << oid 
          << " " << start << "~" << length
	  << " (bl is " << bl.length() << ")"
          << dendl;

  if (bl.length() < length) {
    bufferptr bp(length - bl.length());
    bp.zero();
    dout(7) << "bh_read_finish " << oid << " padding " << start << "~" << length 
	    << " with " << bp.length() << " bytes of zeroes" << dendl;
    bl.push_back(bp);
  }
  
  if (objects.count(oid) == 0) {
    dout(7) << "bh_read_finish no object cache" << dendl;
  } else {
    Object *ob = objects[oid];
    
    // apply to bh's!
    off_t opos = start;
    map<off_t, BufferHead*>::iterator p = ob->data.lower_bound(opos);
    
    while (p != ob->data.end() &&
           opos < start+(off_t)length) {
      BufferHead *bh = p->second;
      
      if (bh->start() > opos) {
        dout(1) << "weirdness: gap when applying read results, " 
                << opos << "~" << bh->start() - opos 
                << dendl;
        opos = bh->start();
        continue;
      }
      
      if (!bh->is_rx()) {
        dout(10) << "bh_read_finish skipping non-rx " << *bh << dendl;
        opos = bh->end();
        p++;
        continue;
      }
      
      assert(opos >= bh->start());
      assert(bh->start() == opos);   // we don't merge rx bh's... yet!
      assert(bh->length() <= start+(off_t)length-opos);
      
      bh->bl.substr_of(bl,
                       opos-bh->start(),
                       bh->length());
      mark_clean(bh);
      dout(10) << "bh_read_finish read " << *bh << dendl;
      
      opos = bh->end();
      p++;

      // finishers?
      // called with lock held.
      list<Context*> ls;
      for (map<off_t, list<Context*> >::iterator p = bh->waitfor_read.begin();
           p != bh->waitfor_read.end();
           p++)
        ls.splice(ls.end(), p->second);
      bh->waitfor_read.clear();
      finish_contexts(ls);

      // clean up?
      ob->try_merge_bh(bh);
    }
  }
  //lock.Unlock();
}


void ObjectCacher::bh_write(BufferHead *bh)
{
  dout(7) << "bh_write " << *bh << dendl;
  
  // finishers
  C_WriteAck *onack = new C_WriteAck(this, bh->ob->get_oid(), bh->start(), bh->length());
  C_WriteCommit *oncommit = new C_WriteCommit(this, bh->ob->get_oid(), bh->start(), bh->length());

  // go
  tid_t tid = objecter->write(bh->ob->get_oid(), bh->start(), bh->length(), bh->ob->get_layout(), bh->bl,
                              onack, oncommit);

  // set bh last_write_tid
  onack->tid = tid;
  oncommit->tid = tid;
  bh->ob->last_write_tid = tid;
  bh->last_write_tid = tid;

  mark_tx(bh);
}

void ObjectCacher::lock_ack(list<object_t>& oids, tid_t tid)
{
  for (list<object_t>::iterator i = oids.begin();
       i != oids.end();
       i++) {
    object_t oid = *i;

    if (objects.count(oid) == 0) {
      dout(7) << "lock_ack no object cache" << dendl;
      assert(0);
    } 
    
    Object *ob = objects[oid];

    list<Context*> ls;
    
    assert(tid <= ob->last_write_tid);
    if (ob->last_write_tid == tid) {
      dout(10) << "lock_ack " << *ob
               << " tid " << tid << dendl;

      switch (ob->lock_state) {
      case Object::LOCK_RDUNLOCKING: 
      case Object::LOCK_WRUNLOCKING: 
        ob->lock_state = Object::LOCK_NONE; 
        break;
      case Object::LOCK_RDLOCKING: 
      case Object::LOCK_DOWNGRADING: 
        ob->lock_state = Object::LOCK_RDLOCK; 
        ls.splice(ls.begin(), ob->waitfor_rd);
        break;
      case Object::LOCK_UPGRADING: 
      case Object::LOCK_WRLOCKING: 
        ob->lock_state = Object::LOCK_WRLOCK; 
        ls.splice(ls.begin(), ob->waitfor_wr);
        ls.splice(ls.begin(), ob->waitfor_rd);
        break;

      default:
        assert(0);
      }
      
      ob->last_ack_tid = tid;
      
      if (ob->can_close())
        close_object(ob);
    } else {
      dout(10) << "lock_ack " << *ob 
               << " tid " << tid << " obsolete" << dendl;
    }

    // waiters?
    if (ob->waitfor_ack.count(tid)) {
      ls.splice(ls.end(), ob->waitfor_ack[tid]);
      ob->waitfor_ack.erase(tid);
    }

    finish_contexts(ls);

  }
}

void ObjectCacher::bh_write_ack(object_t oid, off_t start, size_t length, tid_t tid)
{
  //lock.Lock();
  
  dout(7) << "bh_write_ack " 
          << oid 
          << " tid " << tid
          << " " << start << "~" << length
          << dendl;
  if (objects.count(oid) == 0) {
    dout(7) << "bh_write_ack no object cache" << dendl;
    assert(0);
  } else {
    Object *ob = objects[oid];
    
    // apply to bh's!
    for (map<off_t, BufferHead*>::iterator p = ob->data.lower_bound(start);
         p != ob->data.end();
         p++) {
      BufferHead *bh = p->second;
      
      if (bh->start() > start+(off_t)length) break;

      if (bh->start() < start &&
          bh->end() > start+(off_t)length) {
        dout(20) << "bh_write_ack skipping " << *bh << dendl;
        continue;
      }
      
      // make sure bh is tx
      if (!bh->is_tx()) {
        dout(10) << "bh_write_ack skipping non-tx " << *bh << dendl;
        continue;
      }
      
      // make sure bh tid matches
      if (bh->last_write_tid != tid) {
        assert(bh->last_write_tid > tid);
        dout(10) << "bh_write_ack newer tid on " << *bh << dendl;
        continue;
      }
      
      // ok!  mark bh clean.
      mark_clean(bh);
      dout(10) << "bh_write_ack clean " << *bh << dendl;
    }
    
    // update object last_ack.
    assert(ob->last_ack_tid < tid);
    ob->last_ack_tid = tid;
    
    // waiters?
    if (ob->waitfor_ack.count(tid)) {
      list<Context*> ls;
      ls.splice(ls.begin(), ob->waitfor_ack[tid]);
      ob->waitfor_ack.erase(tid);
      finish_contexts(ls);
    }
  }
  //lock.Unlock();
}

void ObjectCacher::bh_write_commit(object_t oid, off_t start, size_t length, tid_t tid)
{
  //lock.Lock();
  
  // update object last_commit
  dout(7) << "bh_write_commit " 
          << oid 
          << " tid " << tid
          << " " << start << "~" << length
          << dendl;
  if (objects.count(oid) == 0) {
    dout(7) << "bh_write_commit no object cache" << dendl;
    //assert(0);
  } else {
    Object *ob = objects[oid];
    
    // update last_commit.
    ob->last_commit_tid = tid;
    
    // waiters?
    if (ob->waitfor_commit.count(tid)) {
      list<Context*> ls;
      ls.splice(ls.begin(), ob->waitfor_commit[tid]);
      ob->waitfor_commit.erase(tid);
      finish_contexts(ls);
    }
  }

  //  lock.Unlock();
}


void ObjectCacher::flush(off_t amount)
{
  utime_t cutoff = g_clock.now();
  //cutoff.sec_ref() -= g_conf.client_oc_max_dirty_age;

  dout(10) << "flush " << amount << dendl;
  
  /*
   * NOTE: we aren't actually pulling things off the LRU here, just looking at the
   * tail item.  Then we call bh_write, which moves it to the other LRU, so that we
   * can call lru_dirty.lru_get_next_expire() again.
   */
  off_t did = 0;
  while (amount == 0 || did < amount) {
    BufferHead *bh = (BufferHead*) lru_dirty.lru_get_next_expire();
    if (!bh) break;
    if (bh->last_write > cutoff) break;

    did += bh->length();
    bh_write(bh);
  }    
}


void ObjectCacher::trim(off_t max)
{
  if (max < 0) 
    max = g_conf.client_oc_size;
  
  dout(10) << "trim  start: max " << max 
           << "  clean " << get_stat_clean()
           << dendl;

  while (get_stat_clean() > max) {
    BufferHead *bh = (BufferHead*) lru_rest.lru_expire();
    if (!bh) break;
    
    dout(10) << "trim trimming " << *bh << dendl;
    assert(bh->is_clean());
    
    Object *ob = bh->ob;
    bh_remove(ob, bh);
    delete bh;
    
    if (ob->can_close()) {
      dout(10) << "trim trimming " << *ob << dendl;
      close_object(ob);
    }
  }
  
  dout(10) << "trim finish: max " << max 
           << "  clean " << get_stat_clean()
           << dendl;
}



/* public */

/*
 * returns # bytes read (if in cache).  onfinish is untouched (caller must delete it)
 * returns 0 if doing async read
 */
int ObjectCacher::readx(Objecter::OSDRead *rd, inodeno_t ino, Context *onfinish)
{
  bool success = true;
  list<BufferHead*> hit_ls;
  map<size_t, bufferlist> stripe_map;  // final buffer offset -> substring

  for (list<ObjectExtent>::iterator ex_it = rd->extents.begin();
       ex_it != rd->extents.end();
       ex_it++) {
    dout(10) << "readx " << *ex_it << dendl;

    // get Object cache
    Object *o = get_object(ex_it->oid, ino, ex_it->layout);
    
    // map extent into bufferheads
    map<off_t, BufferHead*> hits, missing, rx;
    o->map_read(rd, hits, missing, rx);
    
    if (!missing.empty() || !rx.empty()) {
      // read missing
      for (map<off_t, BufferHead*>::iterator bh_it = missing.begin();
           bh_it != missing.end();
           bh_it++) {
        bh_read(bh_it->second);
        if (success) {
          dout(10) << "readx missed, waiting on " << *bh_it->second 
                   << " off " << bh_it->first << dendl;
          success = false;
          bh_it->second->waitfor_read[bh_it->first].push_back( new C_RetryRead(this, rd, ino, onfinish) );
        }
      }

      // bump rx
      for (map<off_t, BufferHead*>::iterator bh_it = rx.begin();
           bh_it != rx.end();
           bh_it++) {
        touch_bh(bh_it->second);        // bump in lru, so we don't lose it.
        if (success) {
          dout(10) << "readx missed, waiting on " << *bh_it->second 
                   << " off " << bh_it->first << dendl;
          success = false;
          bh_it->second->waitfor_read[bh_it->first].push_back( new C_RetryRead(this, rd, ino, onfinish) );
        }
      }      
    } else {
      assert(!hits.empty());

      // make a plain list
      for (map<off_t, BufferHead*>::iterator bh_it = hits.begin();
           bh_it != hits.end();
           bh_it++) {
	dout(10) << "readx hit bh " << *bh_it->second << dendl;
        hit_ls.push_back(bh_it->second);
      }

      // create reverse map of buffer offset -> object for the eventual result.
      // this is over a single ObjectExtent, so we know that
      //  - the bh's are contiguous
      //  - the buffer frags need not be (and almost certainly aren't)
      off_t opos = ex_it->start;
      map<off_t, BufferHead*>::iterator bh_it = hits.begin();
      assert(bh_it->second->start() <= opos);
      size_t bhoff = opos - bh_it->second->start();
      map<size_t,size_t>::iterator f_it = ex_it->buffer_extents.begin();
      size_t foff = 0;
      while (1) {
        BufferHead *bh = bh_it->second;
        assert(opos == (off_t)(bh->start() + bhoff));

        dout(10) << "readx rmap opos " << opos
                 << ": " << *bh << " +" << bhoff
                 << " frag " << f_it->first << "~" << f_it->second << " +" << foff
                 << dendl;

        size_t len = MIN(f_it->second - foff,
                         bh->length() - bhoff);
	bufferlist bit;  // put substr here first, since substr_of clobbers, and
	                 // we may get multiple bh's at this stripe_map position
	bit.substr_of(bh->bl,
		      opos - bh->start(),
		      len);
        stripe_map[f_it->first].claim_append(bit);

        opos += len;
        bhoff += len;
        foff += len;
        if (opos == bh->end()) {
          bh_it++;
          bhoff = 0;
        }
        if (foff == f_it->second) {
          f_it++;
          foff = 0;
        }
        if (bh_it == hits.end()) break;
        if (f_it == ex_it->buffer_extents.end()) break;
      }
      assert(f_it == ex_it->buffer_extents.end());
      assert(opos == ex_it->start + (off_t)ex_it->length);
    }
  }
  
  // bump hits in lru
  for (list<BufferHead*>::iterator bhit = hit_ls.begin();
       bhit != hit_ls.end();
       bhit++) 
    touch_bh(*bhit);
  
  if (!success) return 0;  // wait!

  // no misses... success!  do the read.
  assert(!hit_ls.empty());
  dout(10) << "readx has all buffers" << dendl;
  
  // ok, assemble into result buffer.
  rd->bl->clear();
  size_t pos = 0;
  for (map<size_t,bufferlist>::iterator i = stripe_map.begin();
       i != stripe_map.end();
       i++) {
    assert(pos == i->first);
    dout(10) << "readx  adding buffer len " << i->second.length() << " at " << pos << dendl;
    pos += i->second.length();
    rd->bl->claim_append(i->second);
    assert(rd->bl->length() == pos);
  }
  dout(10) << "readx  result is " << rd->bl->length() << dendl;

  // done with read.
  delete rd;

  trim();
  
  return pos;
}


int ObjectCacher::writex(Objecter::OSDWrite *wr, inodeno_t ino)
{
  utime_t now = g_clock.now();
  
  for (list<ObjectExtent>::iterator ex_it = wr->extents.begin();
       ex_it != wr->extents.end();
       ex_it++) {
    // get object cache
    Object *o = get_object(ex_it->oid, ino, ex_it->layout);

    // map it all into a single bufferhead.
    BufferHead *bh = o->map_write(wr);
    
    // adjust buffer pointers (ie "copy" data into my cache)
    // this is over a single ObjectExtent, so we know that
    //  - there is one contiguous bh
    //  - the buffer frags need not be (and almost certainly aren't)
    // note: i assume striping is monotonic... no jumps backwards, ever!
    off_t opos = ex_it->start;
    for (map<size_t,size_t>::iterator f_it = ex_it->buffer_extents.begin();
         f_it != ex_it->buffer_extents.end();
         f_it++) {
      dout(10) << "writex writing " << f_it->first << "~" << f_it->second << " into " << *bh << " at " << opos << dendl;
      size_t bhoff = bh->start() - opos;
      assert(f_it->second <= bh->length() - bhoff);

      // get the frag we're mapping in
      bufferlist frag; 
      frag.substr_of(wr->bl, 
                     f_it->first, f_it->second);

      // keep anything left of bhoff
      bufferlist newbl;
      if (bhoff)
	newbl.substr_of(bh->bl, 0, bhoff);
      newbl.claim_append(frag);
      bh->bl.swap(newbl);

      opos += f_it->second;
    }

    // ok, now bh is dirty.
    mark_dirty(bh);
    touch_bh(bh);
    bh->last_write = now;

    o->try_merge_bh(bh);
  }

  delete wr;

  trim();
  return 0;
}
 

// blocking wait for write.
void ObjectCacher::wait_for_write(size_t len, Mutex& lock)
{
  while (get_stat_dirty() + get_stat_tx() >= g_conf.client_oc_max_dirty) {
    dout(10) << "wait_for_write waiting on " << len << ", dirty|tx " 
	     << (get_stat_dirty() + get_stat_tx()) 
	     << " >= " << g_conf.client_oc_max_dirty 
	     << dendl;
    flusher_cond.Signal();
    stat_waiter++;
    stat_cond.Wait(lock);
    stat_waiter--;
    dout(10) << "wait_for_write woke up" << dendl;
  }
}

void ObjectCacher::flusher_entry()
{
  dout(10) << "flusher start" << dendl;
  lock.Lock();
  while (!flusher_stop) {
    while (!flusher_stop) {
      off_t all = get_stat_tx() + get_stat_rx() + get_stat_clean() + get_stat_dirty();
      dout(11) << "flusher "
               << all << " / " << g_conf.client_oc_size << ":  "
               << get_stat_tx() << " tx, "
               << get_stat_rx() << " rx, "
               << get_stat_clean() << " clean, "
               << get_stat_dirty() << " / " << g_conf.client_oc_max_dirty << " dirty"
               << dendl;
      if (get_stat_dirty() > g_conf.client_oc_max_dirty) {
        // flush some dirty pages
        dout(10) << "flusher " 
                 << get_stat_dirty() << " / " << g_conf.client_oc_max_dirty << " dirty,"
                 << " flushing some dirty bhs" << dendl;
        flush(get_stat_dirty() - g_conf.client_oc_max_dirty);
      }
      else {
        // check tail of lru for old dirty items
        utime_t cutoff = g_clock.now();
        cutoff.sec_ref()--;
        BufferHead *bh = 0;
        while ((bh = (BufferHead*)lru_dirty.lru_get_next_expire()) != 0 &&
               bh->last_write < cutoff) {
          dout(10) << "flusher flushing aged dirty bh " << *bh << dendl;
          bh_write(bh);
        }
        break;
      }
    }
    if (flusher_stop) break;
    flusher_cond.WaitInterval(lock, utime_t(1,0));
  }
  lock.Unlock();
  dout(10) << "flusher finish" << dendl;
}


  
// blocking.  atomic+sync.
int ObjectCacher::atomic_sync_readx(Objecter::OSDRead *rd, inodeno_t ino, Mutex& lock)
{
  dout(10) << "atomic_sync_readx " << rd
           << " in " << ino
           << dendl;

  if (rd->extents.size() == 1) {
    // single object.
    // just write synchronously.
    Cond cond;
    bool done = false;
    objecter->readx(rd, new C_SafeCond(&lock, &cond, &done));

    // block
    while (!done) cond.Wait(lock);
  } else {
    // spans multiple objects, or is big.

    // sort by object...
    map<object_t,ObjectExtent> by_oid;
    for (list<ObjectExtent>::iterator ex_it = rd->extents.begin();
         ex_it != rd->extents.end();
         ex_it++) 
      by_oid[ex_it->oid] = *ex_it;
    
    // lock
    for (map<object_t,ObjectExtent>::iterator i = by_oid.begin();
         i != by_oid.end();
         i++) {
      Object *o = get_object(i->first, ino, i->second.layout);
      rdlock(o);
    }

    // readx will hose rd
    list<ObjectExtent> extents = rd->extents;

    // do the read, into our cache
    Cond cond;
    bool done = false;
    readx(rd, ino, new C_SafeCond(&lock, &cond, &done));
    
    // block
    while (!done) cond.Wait(lock);
    
    // release the locks
    for (list<ObjectExtent>::iterator ex_it = extents.begin();
         ex_it != extents.end();
         ex_it++) {
      assert(objects.count(ex_it->oid));
      Object *o = objects[ex_it->oid];
      rdunlock(o);
    }
  }

  return 0;
}

int ObjectCacher::atomic_sync_writex(Objecter::OSDWrite *wr, inodeno_t ino, Mutex& lock)
{
  dout(10) << "atomic_sync_writex " << wr
           << " in " << ino
           << dendl;

  if (wr->extents.size() == 1 &&
      wr->extents.front().length <= g_conf.client_oc_max_sync_write) {
    // single object.
    
    // make sure we aren't already locking/locked...
    object_t oid = wr->extents.front().oid;
    Object *o = 0;
    if (objects.count(oid)) o = get_object(oid, ino, wr->extents.front().layout);
    if (!o || 
        (o->lock_state != Object::LOCK_WRLOCK &&
         o->lock_state != Object::LOCK_WRLOCKING &&
         o->lock_state != Object::LOCK_UPGRADING)) {
      // just write synchronously.
      dout(10) << "atomic_sync_writex " << wr
               << " in " << ino
               << " doing sync write"
               << dendl;

      Cond cond;
      bool done = false;
      objecter->modifyx(wr, new C_SafeCond(&lock, &cond, &done), 0);
      
      // block
      while (!done) cond.Wait(lock);
      return 0;
    }
  } 

  // spans multiple objects, or is big.
  // sort by object...
  map<object_t,ObjectExtent> by_oid;
  for (list<ObjectExtent>::iterator ex_it = wr->extents.begin();
       ex_it != wr->extents.end();
       ex_it++) 
    by_oid[ex_it->oid] = *ex_it;
  
  // wrlock
  for (map<object_t,ObjectExtent>::iterator i = by_oid.begin();
       i != by_oid.end();
       i++) {
    Object *o = get_object(i->first, ino, i->second.layout);
    wrlock(o);
  }
  
  // writex will hose wr
  list<ObjectExtent> extents = wr->extents;

  // do the write, into our cache
  writex(wr, ino);
  
  // flush 
  // ...and release the locks?
  for (list<ObjectExtent>::iterator ex_it = extents.begin();
       ex_it != extents.end();
       ex_it++) {
    assert(objects.count(ex_it->oid));
    Object *o = objects[ex_it->oid];
    
    wrunlock(o);
  }

  return 0;
}
 


// locking -----------------------------

void ObjectCacher::rdlock(Object *o)
{
  // lock?
  if (o->lock_state == Object::LOCK_NONE ||
      o->lock_state == Object::LOCK_RDUNLOCKING ||
      o->lock_state == Object::LOCK_WRUNLOCKING) {
    dout(10) << "rdlock rdlock " << *o << dendl;
    
    o->lock_state = Object::LOCK_RDLOCKING;
    
    C_LockAck *ack = new C_LockAck(this, o->get_oid());
    C_WriteCommit *commit = new C_WriteCommit(this, o->get_oid(), 0, 0);
    
    commit->tid = 
      ack->tid = 
      o->last_write_tid = 
      objecter->lock(CEPH_OSD_OP_RDLOCK, o->get_oid(), o->get_layout(), ack, commit);
  }
  
  // stake our claim.
  o->rdlock_ref++;  
  
  // wait?
  if (o->lock_state == Object::LOCK_RDLOCKING ||
      o->lock_state == Object::LOCK_WRLOCKING) {
    dout(10) << "rdlock waiting for rdlock|wrlock on " << *o << dendl;
    Cond cond;
    bool done = false;
    o->waitfor_rd.push_back(new C_SafeCond(&lock, &cond, &done));
    while (!done) cond.Wait(lock);
  }
  assert(o->lock_state == Object::LOCK_RDLOCK ||
         o->lock_state == Object::LOCK_WRLOCK ||
         o->lock_state == Object::LOCK_UPGRADING ||
         o->lock_state == Object::LOCK_DOWNGRADING);
}

void ObjectCacher::wrlock(Object *o)
{
  // lock?
  if (o->lock_state != Object::LOCK_WRLOCK &&
      o->lock_state != Object::LOCK_WRLOCKING &&
      o->lock_state != Object::LOCK_UPGRADING) {
    dout(10) << "wrlock wrlock " << *o << dendl;
    
    int op = 0;
    if (o->lock_state == Object::LOCK_RDLOCK) {
      o->lock_state = Object::LOCK_UPGRADING;
      op = CEPH_OSD_OP_UPLOCK;
    } else {
      o->lock_state = Object::LOCK_WRLOCKING;
      op = CEPH_OSD_OP_WRLOCK;
    }
    
    C_LockAck *ack = new C_LockAck(this, o->get_oid());
    C_WriteCommit *commit = new C_WriteCommit(this, o->get_oid(), 0, 0);
    
    commit->tid = 
      ack->tid = 
      o->last_write_tid = 
      objecter->lock(op, o->get_oid(), o->get_layout(), ack, commit);
  }
  
  // stake our claim.
  o->wrlock_ref++;  
  
  // wait?
  if (o->lock_state == Object::LOCK_WRLOCKING ||
      o->lock_state == Object::LOCK_UPGRADING) {
    dout(10) << "wrlock waiting for wrlock on " << *o << dendl;
    Cond cond;
    bool done = false;
    o->waitfor_wr.push_back(new C_SafeCond(&lock, &cond, &done));
    while (!done) cond.Wait(lock);
  }
  assert(o->lock_state == Object::LOCK_WRLOCK);
}


void ObjectCacher::rdunlock(Object *o)
{
  dout(10) << "rdunlock " << *o << dendl;
  assert(o->lock_state == Object::LOCK_RDLOCK ||
         o->lock_state == Object::LOCK_WRLOCK ||
         o->lock_state == Object::LOCK_UPGRADING ||
         o->lock_state == Object::LOCK_DOWNGRADING);

  assert(o->rdlock_ref > 0);
  o->rdlock_ref--;
  if (o->rdlock_ref > 0 ||
      o->wrlock_ref > 0) {
    dout(10) << "rdunlock " << *o << " still has rdlock|wrlock refs" << dendl;
    return;
  }

  release(o);  // release first

  o->lock_state = Object::LOCK_RDUNLOCKING;

  C_LockAck *lockack = new C_LockAck(this, o->get_oid());
  C_WriteCommit *commit = new C_WriteCommit(this, o->get_oid(), 0, 0);
  commit->tid = 
    lockack->tid = 
    o->last_write_tid = 
    objecter->lock(CEPH_OSD_OP_RDUNLOCK, o->get_oid(), o->get_layout(), lockack, commit);
}

void ObjectCacher::wrunlock(Object *o)
{
  dout(10) << "wrunlock " << *o << dendl;
  assert(o->lock_state == Object::LOCK_WRLOCK);

  assert(o->wrlock_ref > 0);
  o->wrlock_ref--;
  if (o->wrlock_ref > 0) {
    dout(10) << "wrunlock " << *o << " still has wrlock refs" << dendl;
    return;
  }

  flush(o);  // flush first

  int op = 0;
  if (o->rdlock_ref > 0) {
    dout(10) << "wrunlock rdlock " << *o << dendl;
    op = CEPH_OSD_OP_DNLOCK;
    o->lock_state = Object::LOCK_DOWNGRADING;
  } else {
    dout(10) << "wrunlock wrunlock " << *o << dendl;
    op = CEPH_OSD_OP_WRUNLOCK;
    o->lock_state = Object::LOCK_WRUNLOCKING;
  }

  C_LockAck *lockack = new C_LockAck(this, o->get_oid());
  C_WriteCommit *commit = new C_WriteCommit(this, o->get_oid(), 0, 0);
  commit->tid = 
    lockack->tid = 
    o->last_write_tid = 
    objecter->lock(op, o->get_oid(), o->get_layout(), lockack, commit);
}


// -------------------------------------------------


bool ObjectCacher::set_is_cached(inodeno_t ino)
{
  if (objects_by_ino.count(ino) == 0) 
    return false;
  
  set<Object*>& s = objects_by_ino[ino];
  for (set<Object*>::iterator i = s.begin();
       i != s.end();
       i++) {
    Object *ob = *i;
    if (!ob->data.empty()) return true;
  }

  return false;
}

bool ObjectCacher::set_is_dirty_or_committing(inodeno_t ino)
{
  if (objects_by_ino.count(ino) == 0) 
    return false;
  
  set<Object*>& s = objects_by_ino[ino];
  for (set<Object*>::iterator i = s.begin();
       i != s.end();
       i++) {
    Object *ob = *i;

    for (map<off_t,BufferHead*>::iterator p = ob->data.begin();
         p != ob->data.end();
         p++) {
      BufferHead *bh = p->second;
      if (bh->is_dirty() || bh->is_tx()) 
        return true;
    }
  }  

  return false;
}


// purge.  non-blocking.  violently removes dirty buffers from cache.
void ObjectCacher::purge(Object *ob)
{
  dout(10) << "purge " << *ob << dendl;

  for (map<off_t,BufferHead*>::iterator p = ob->data.begin();
       p != ob->data.end();
       p++) {
    BufferHead *bh = p->second;
	if (!bh->is_clean())
	  dout(0) << "purge forcibly removing " << *ob << " " << *bh << dendl;
	bh_remove(ob, bh);
	delete bh;
  }

  if (ob->can_close()) {
	dout(10) << "trim trimming " << *ob << dendl;
	close_object(ob);
  }
}

// flush.  non-blocking.  no callback.
// true if clean, already flushed.  
// false if we wrote something.
bool ObjectCacher::flush(Object *ob)
{
  bool clean = true;
  for (map<off_t,BufferHead*>::iterator p = ob->data.begin();
       p != ob->data.end();
       p++) {
    BufferHead *bh = p->second;
    if (bh->is_tx()) {
      clean = false;
      continue;
    }
    if (!bh->is_dirty()) continue;
    
    bh_write(bh);
    clean = false;
  }
  return clean;
}

// flush.  non-blocking, takes callback.
// returns true if already flushed
bool ObjectCacher::flush_set(inodeno_t ino, Context *onfinish)
{
  if (objects_by_ino.count(ino) == 0) {
    dout(10) << "flush_set on " << ino << " dne" << dendl;
    return true;
  }

  dout(10) << "flush_set " << ino << dendl;

  C_Gather *gather = 0; // we'll need to wait for all objects to flush!

  set<Object*>& s = objects_by_ino[ino];
  bool safe = true;
  for (set<Object*>::iterator i = s.begin();
       i != s.end();
       i++) {
    Object *ob = *i;

    if (!flush(ob)) {
      // we'll need to gather...
      if (!gather && onfinish) 
        gather = new C_Gather(onfinish);
      safe = false;

      dout(10) << "flush_set " << ino << " will wait for ack tid " 
               << ob->last_write_tid 
               << " on " << *ob
               << dendl;
      if (gather)
        ob->waitfor_ack[ob->last_write_tid].push_back(gather->new_sub());
    }
  }
  
  if (safe) {
    dout(10) << "flush_set " << ino << " has no dirty|tx bhs" << dendl;
    return true;
  }
  return false;
}


// commit.  non-blocking, takes callback.
// return true if already flushed.
bool ObjectCacher::commit_set(inodeno_t ino, Context *onfinish)
{
  assert(onfinish);  // doesn't make any sense otherwise.

  if (objects_by_ino.count(ino) == 0) {
    dout(10) << "commit_set on " << ino << " dne" << dendl;
    return true;
  }

  dout(10) << "commit_set " << ino << dendl;

  C_Gather *gather = 0; // we'll need to wait for all objects to commit

  set<Object*>& s = objects_by_ino[ino];
  bool safe = true;
  for (set<Object*>::iterator i = s.begin();
       i != s.end();
       i++) {
    Object *ob = *i;
    
    // make sure it's flushing.
    flush_set(ino);

    if (ob->last_write_tid > ob->last_commit_tid) {
      dout(10) << "commit_set " << ino << " " << *ob 
               << " will finish on commit tid " << ob->last_write_tid
               << dendl;
      if (!gather && onfinish) gather = new C_Gather(onfinish);
      safe = false;
      if (gather)
        ob->waitfor_commit[ob->last_write_tid].push_back( gather->new_sub() );
    }
  }

  if (safe) {
    dout(10) << "commit_set " << ino << " all committed" << dendl;
    return true;
  }
  return false;
}

void ObjectCacher::purge_set(inodeno_t ino)
{
  if (objects_by_ino.count(ino) == 0) {
    dout(10) << "purge_set on " << ino << " dne" << dendl;
    return;
  }

  dout(10) << "purge_set " << ino << dendl;

  set<Object*>& s = objects_by_ino[ino];
  for (set<Object*>::iterator i = s.begin();
       i != s.end();
       i++) {
    Object *ob = *i;
	purge(ob);
  }
}


off_t ObjectCacher::release(Object *ob)
{
  list<BufferHead*> clean;
  off_t o_unclean = 0;

  for (map<off_t,BufferHead*>::iterator p = ob->data.begin();
       p != ob->data.end();
       p++) {
    BufferHead *bh = p->second;
    if (bh->is_clean()) 
	  clean.push_back(bh);
    else 
      o_unclean += bh->length();
  }

  for (list<BufferHead*>::iterator p = clean.begin();
	   p != clean.end();
	   p++) {
	bh_remove(ob, *p);
	delete *p;
  }

  if (ob->can_close()) {
	dout(10) << "trim trimming " << *ob << dendl;
	close_object(ob);
  }

  return o_unclean;
}

off_t ObjectCacher::release_set(inodeno_t ino)
{
  // return # bytes not clean (and thus not released).
  off_t unclean = 0;

  if (objects_by_ino.count(ino) == 0) {
    dout(10) << "release_set on " << ino << " dne" << dendl;
    return 0;
  }

  dout(10) << "release_set " << ino << dendl;

  set<Object*> s = objects_by_ino[ino];
  for (set<Object*>::iterator i = s.begin();
       i != s.end();
       i++) {
    Object *ob = *i;
    
    off_t o_unclean = release(ob);
    unclean += o_unclean;

    if (o_unclean) 
      dout(10) << "release_set " << ino << " " << *ob 
               << " has " << o_unclean << " bytes left"
               << dendl;
    
  }

  if (unclean) {
    dout(10) << "release_set " << ino
             << ", " << unclean << " bytes left" << dendl;
  }

  return unclean;
}

void ObjectCacher::truncate_set(inodeno_t ino, list<ObjectExtent>& exls)
{
  if (objects_by_ino.count(ino) == 0) {
    dout(10) << "truncate_set on " << ino << " dne" << dendl;
    return;
  }
  
  dout(10) << "truncate_set " << ino << dendl;

  for (list<ObjectExtent>::iterator p = exls.begin();
	   p != exls.end();
	   ++p) {
	ObjectExtent &ex = *p;
	if (objects.count(ex.oid) == 0) continue;
	Object *ob = objects[ex.oid];

	// purge or truncate?
	if (ex.start == 0) {
	  dout(10) << "truncate_set purging " << *ob << dendl;
	  purge(ob);
	} else {
	  // hrm, truncate object
	  dout(10) << "truncate_set truncating " << *ob << " at " << ex.start << dendl;
	  ob->truncate(ex.start);

	  if (ob->can_close()) {
		dout(10) << "truncate_set trimming " << *ob << dendl;
		close_object(ob);
	  }
	}
  }
}


void ObjectCacher::kick_sync_writers(inodeno_t ino)
{
  if (objects_by_ino.count(ino) == 0) {
    dout(10) << "kick_sync_writers on " << ino << " dne" << dendl;
    return;
  }

  dout(10) << "kick_sync_writers on " << ino << dendl;

  list<Context*> ls;

  set<Object*>& s = objects_by_ino[ino];
  for (set<Object*>::iterator i = s.begin();
       i != s.end();
       i++) {
    Object *ob = *i;
    
    ls.splice(ls.begin(), ob->waitfor_wr);
  }

  finish_contexts(ls);
}

void ObjectCacher::kick_sync_readers(inodeno_t ino)
{
  if (objects_by_ino.count(ino) == 0) {
    dout(10) << "kick_sync_readers on " << ino << " dne" << dendl;
    return;
  }

  dout(10) << "kick_sync_readers on " << ino << dendl;

  list<Context*> ls;

  set<Object*>& s = objects_by_ino[ino];
  for (set<Object*>::iterator i = s.begin();
       i != s.end();
       i++) {
    Object *ob = *i;
    
    ls.splice(ls.begin(), ob->waitfor_rd);
  }

  finish_contexts(ls);
}



