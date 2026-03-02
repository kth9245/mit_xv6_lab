// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13
struct bucket {
  struct spinlock lock;
  struct buf *buf;
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct bucket buckets[NBUCKET];
  uint clock;
} bcache;

#define HASH(dev, blockno) (((dev) ^ (blockno)) % NBUCKET)

void insert(struct buf *b){
  int idx = HASH(b->dev, b->blockno);
  acquire(&bcache.buckets[idx].lock);
  b->next = bcache.buckets[idx].buf;
  bcache.buckets[idx].buf = b;
  release(&bcache.buckets[idx].lock);
}

void
binit(void)
{
  struct buf *b;
  static char lock_name[NBUCKET][16];
  initlock(&bcache.lock, "bcache");
  bcache.clock = 1;

  for (int i = 0; i < NBUCKET; i++) {
    snprintf(lock_name[i], sizeof(lock_name[i]), "bcache.bucket%d", i);
    initlock(&bcache.buckets[i].lock, lock_name[i]);
    bcache.buckets[i].buf = 0;
  }

  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    b->valid = 0;
    b->dev = 0;
    b->blockno = 0;
    b->refcnt = 0;
    b->ts = 0;
    b->prev = 0;
    b->next = 0;
    insert(b);
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct buf *victim;
  struct buf *victim_prev;
  int victim_idx;
  uint oldest;

  int idx = HASH(dev, blockno);

  // Fast path: search only the target bucket.
  acquire(&bcache.buckets[idx].lock);
  for(b = bcache.buckets[idx].buf; b != 0; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.buckets[idx].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.buckets[idx].lock);

  // Miss path: serialize eviction and movement across buckets.
  acquire(&bcache.lock);

  // Re-check after taking bcache.lock to avoid duplicate buffers.
  acquire(&bcache.buckets[idx].lock);
  for(b = bcache.buckets[idx].buf; b != 0; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.buckets[idx].lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.buckets[idx].lock);

  for(;;) {
    victim = 0;
    victim_prev = 0;
    victim_idx = -1;
    oldest = (uint)-1;

    // Pick the oldest free buffer from all buckets.
    for(int i = 0; i < NBUCKET; i++) {
      struct buf *prev = 0;
      acquire(&bcache.buckets[i].lock);
      for(b = bcache.buckets[i].buf; b != 0; b = b->next) {
        if(b->refcnt == 0 && b->ts <= oldest) {
          oldest = b->ts;
          victim = b;
          victim_prev = prev;
          victim_idx = i;
        }
        prev = b;
      }
      release(&bcache.buckets[i].lock);
    }

    if(victim == 0) {
      release(&bcache.lock);
      panic("bget: no buffers");
    }

    // Remove victim from original bucket.
    acquire(&bcache.buckets[victim_idx].lock);
    if(victim->refcnt != 0) {
      release(&bcache.buckets[victim_idx].lock);
      continue;
    }

    // victim_prev can be stale; find victim again if needed.
    if(victim_prev == 0 || victim_prev->next != victim) {
      struct buf *prev = 0;
      struct buf *cur = bcache.buckets[victim_idx].buf;
      while(cur && cur != victim) {
        prev = cur;
        cur = cur->next;
      }
      if(cur == 0 || cur->refcnt != 0) {
        release(&bcache.buckets[victim_idx].lock);
        continue;
      }
      victim_prev = prev;
    }

    if(victim_prev)
      victim_prev->next = victim->next;
    else
      bcache.buckets[victim_idx].buf = victim->next;
    release(&bcache.buckets[victim_idx].lock);

    // Install victim into the target bucket.
    acquire(&bcache.buckets[idx].lock);
    // Another CPU cannot insert same block here while bcache.lock is held,
    // but keep this check for robustness.
    for(b = bcache.buckets[idx].buf; b != 0; b = b->next){
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        release(&bcache.buckets[idx].lock);
        release(&bcache.lock);
        acquiresleep(&b->lock);
        return b;
      }
    }

    victim->dev = dev;
    victim->blockno = blockno;
    victim->valid = 0;
    victim->refcnt = 1;
    victim->next = bcache.buckets[idx].buf;
    bcache.buckets[idx].buf = victim;
    release(&bcache.buckets[idx].lock);
    release(&bcache.lock);

    acquiresleep(&victim->lock);
    return victim;
  }
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int idx = HASH(b->dev, b->blockno);
  acquire(&bcache.buckets[idx].lock);
  if(b->refcnt < 1)
    panic("brelse");
  b->refcnt--;
  if(b->refcnt == 0) {
    b->ts = __sync_fetch_and_add(&bcache.clock, 1);
  }

  release(&bcache.buckets[idx].lock);
}

void
bpin(struct buf *b) {
  int idx = HASH(b->dev, b->blockno);
  acquire(&bcache.buckets[idx].lock);
  b->refcnt++;
  release(&bcache.buckets[idx].lock);
}

void
bunpin(struct buf *b) {
  int idx = HASH(b->dev, b->blockno);
  acquire(&bcache.buckets[idx].lock);
  if(b->refcnt < 1)
    panic("bunpin");
  b->refcnt--;
  if(b->refcnt == 0) {
    b->ts = __sync_fetch_and_add(&bcache.clock, 1);
  }
  release(&bcache.buckets[idx].lock);
}
