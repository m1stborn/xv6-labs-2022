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

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
} bcache;

struct {
  struct spinlock lock;
  struct buf head;
} hashtable[NBUCKET];

int
hash_fn(int blockno)
{
  return blockno % NBUCKET;
}

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for(int i=0; i<NBUCKET; i++) {
    initlock(&hashtable[i].lock, "bcache_bucket_lock");
  }

  // Init hashtable
  for (int i = 0; i < NBUCKET; i++){
    hashtable[i].head.next = &hashtable[i].head;
    hashtable[i].head.prev = &hashtable[i].head;
  }

  // Create linked list of buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    // Insert b between head and first node.
    // TODO: equally insert to all bucket, now just insert to bucket[0]
    b->next = hashtable[0].head.next;
    b->prev = &hashtable[0].head;
    initsleeplock(&b->lock, "buffer");
    hashtable[0].head.next->prev = b;
    hashtable[0].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int idx = hash_fn(blockno);
  acquire(&hashtable[idx].lock);

  // Is the block already cached?
  for(b = hashtable[idx].head.next; b != &hashtable[idx].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&hashtable[idx].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // 1. Recycle the least recently used (LRU) unused buffer from `same bucket`!!.
  for(b = hashtable[idx].head.prev; b != &hashtable[idx].head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&hashtable[idx].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 2. Recycle the least recently used (LRU) unused buffer from `other bucket`!!.
  for(int i=0; i<NBUCKET; i++) {
    // lock ordering to avoid deadlock.
    if(i==idx) continue;
    int s_idx = (i<idx)?i:idx, l_idx = (i<idx)?idx:i;
    release(&hashtable[idx].lock); // reset locking order.

    acquire(&hashtable[s_idx].lock);
    acquire(&hashtable[l_idx].lock);

    for(b = hashtable[i].head.next; b != &hashtable[i].head; b = b->next){
      if(b->refcnt == 0) {
        // move the buffer to another bucket
        b->prev->next = b->next;
        b->next->prev = b->prev;
        hashtable[idx].head.next->prev = b;
        b->next = hashtable[idx].head.next;
        hashtable[idx].head.next = b;
        b->prev = &hashtable[idx].head;

        // update buf info
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&hashtable[i].lock);
        release(&hashtable[idx].lock);
        acquiresleep(&b->lock);
        return b;
      }
    }

    release(&hashtable[i].lock); // if not found empty buf, we release lock[i] before move to next.
  }

  panic("bget: no buffers");
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

  int idx = hash_fn(b->blockno);
  acquire(&hashtable[idx].lock);
  --b->refcnt;
  release(&hashtable[idx].lock);

}

void
bpin(struct buf *b) {
  int idx = hash_fn(b->blockno);
  acquire(&hashtable[idx].lock);
  b->refcnt++;
  release(&hashtable[idx].lock);
}

void
bunpin(struct buf *b) {
  int idx = hash_fn(b->blockno);
  acquire(&hashtable[idx].lock);
  b->refcnt--;
  release(&hashtable[idx].lock);
}


