// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  char lock_name[6];
} kmem[NCPU];

void
kinit()
{
  // initlock(&kmem.lock, "kmem");
  for(int i=1; i<NCPU; i++){
    snprintf(kmem[i].lock_name, sizeof(kmem[i].lock_name), "kmem_%d", i);
    initlock(&kmem[i].lock, kmem[i].lock_name);
  }
  // Initially, all free page will be allocated to CPU#0. When kalloc fail, it will try to steal other CPU's free page.
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off(); // disable interrupt
  int cpu_id = cpuid();
  pop_off();  // enable interrupt

  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off(); // disable interrupt
  int cpu_id = cpuid();
  pop_off();  // enable interrupt

  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;

  if(r) kmem[cpu_id].freelist = r->next;
  else {
    for (int i = 0; i < NCPU; i++) {
      if(i == cpu_id) continue; // pass if i == current cpu_id
      acquire(&kmem[i].lock);
      // ==========================================================
      // Method 1. Steal `1` page from other CPU's kmem.freelist
      // r = kmem[i].freelist;
      // if(r) kmem[i].freelist = r->next;
      // ==========================================================
      // ==========================================================
      // Method 2. Steal `half` page from other CPU's kmem.freelist
      if(kmem[i].freelist) {
        struct run *f = kmem[i].freelist; // faster pointer
        struct run *s = kmem[i].freelist; // slow pointer
        while (f && f->next) {
          f = f->next->next;
          s = s->next;
        }
        kmem[cpu_id].freelist = kmem[i].freelist;
        if (s==kmem[i].freelist)  kmem[i].freelist = 0; // s == the only page
        else {
          kmem[i].freelist = s->next;
          s->next = 0;
        }
        r = kmem[cpu_id].freelist;
        kmem[cpu_id].freelist = r->next;
      }
      release(&kmem[i].lock);
      if(r) break;
    }

  }
  release(&kmem[cpu_id].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
