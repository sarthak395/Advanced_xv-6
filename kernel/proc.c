// ASSOCIATED WITH CREATING A PROCESS
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

// linked list implementation of queue
struct node { 
  struct proc *p;
  struct node *next;
};

typedef struct node* nodeptr;

struct node nodes[NPROC]; // making NPROC nodes for processes
nodeptr queues[5]; // 5 queues which contain head  

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void procinit(void)
{
  for(int i=0;i<5;i++) // initialising queues to NULL
    queues[i]=0;
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->mask = -1;
    p->kstack = KSTACK((int)(p - proc));
  }
}


// insert process 'p' at the end of queues[qno]
void push(int qno,struct proc* p)
{
  // ASSIGN A NODE TO PROCESS
  struct node* newnode=0;
  for(int i=0;i<NPROC;i++)
  {
    if(!(nodes[i].p)) // empty node
    {
      newnode=&nodes[i];
      break;
    }
  }

  newnode->p=p;
  newnode->next=0;

  // inserting in queue
  if(queues[qno]==0) // queue is empty
    queues[qno]=newnode;
  else
  {
    struct node* cur=queues[qno];
    while(cur->next){cur=cur->next;}
    cur->next=newnode;
  }
}

// pop and return the first element from queues[qno]
struct proc* pop(int qno)
{
  if(queues[qno]==0) // queue is empty
    return 0;
  
  struct node* del=queues[qno];
  queues[qno]=queues[qno]->next;
  struct proc* ret=del->p;
  del->p=0; // freeing the process 
  return ret;
}

void remove(int qno,int pid)
{
  if(queues[qno]->p->pid==pid){ // we have to remove head
    queues[qno]->p=0; // freeing the node
    queues[qno]=queues[qno]->next;
    return;
  } 

  struct node* cur=queues[qno];
  while(cur && cur->next){
    if(cur->next->p->pid==pid){
      struct node* del=cur->next;
      cur->next=del->next;
      del->p=0;
      return;
    }
    cur=cur->next;
  }
}
// ASSIGN QUEUE TO A PROCESS
void assignQueue(struct proc* p) // assign queue to a process
{
  p->queue=0; // initially the process should be in the highest priority queue
  p->qentertime=ticks+1;
  push(0,p);
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  if ((p->tf_copy = (struct trapframe *)kalloc()) == 0)
  {
    release(&p->lock);
    return 0;
  }

  // time
  p->starttime = ticks; // initialise starting time of process
  p->runtime = 0;
  p->sleeptime = 0;

  // PBS
  p->niceness = 5;    // default
  p->stpriority = 60; // static priority
  p->numpicked = 0;   // number of times picked by scheduler

  // sigalarm and sigreturn
  p->is_sigalarm = 0;
  p->alarmhandler = 0; // function
  p->alarmint = 0;     // alarm interval
  p->tslalarm = 0;     // time since last alarm

  // LBS
  p->tickets = 1; // by default each process has 1 ticket

  // MLFQ
  p->queue=0; // initially process is at queue 0
  p->qentertime=0;

  // FOR WAITX
  p->etime=0;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  if (p->tf_copy)
    kfree((void *)p->tf_copy);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
#ifdef MLFQ
  assignQueue(p);
#endif
  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  np->mask = p->mask;       // copy mask
  np->tickets = p->tickets; // child should have same number of tickets

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  p->etime=ticks;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

int waitx(uint64 addr, uint *rtime, uint *wtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          *rtime = np->runtime;
          *wtime = np->etime - np->starttime - np->runtime;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;

#ifdef RR // ROUND ROBIN SCHEDULER (PRE-EMPTIVE)
  printf("scheduler RR\n");
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }

#elif defined(FCFS) // FIRST COME FIRST SERVE SCHEDULER (NON - PREEMPTIVE)
  printf("scheduler FCFS\n");
  for (;;)
  {
    struct proc *chosenproc = proc;
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // FINDING CHOSEN PROCESS
    int min_time = __INT32_MAX__;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if ((p->state == RUNNABLE) && (p->starttime < min_time))
      {
        chosenproc = p;
        min_time = p->starttime;
      }
    }

    // SWITCHING TO CHOSEN PROC
    acquire(&chosenproc->lock);
    if (chosenproc->state == RUNNABLE)
    {
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      chosenproc->state = RUNNING;
      c->proc = chosenproc;
      swtch(&c->context, &chosenproc->context);

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&chosenproc->lock);
  }

#elif defined(LBS) // LOTTERY BASED SCHEDULER (PRE-EMPTIVE)
  printf("scheduler LBS\n");
  for (;;)
  {
    struct proc *chosenproc = proc;
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // FINDING CHOSEN PROCESS I.E PROCESS WITH MAXIMUM TICKETS
    int max_tickets = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if ((p->state == RUNNABLE) && (p->tickets > max_tickets))
      {
        chosenproc = p;
        max_tickets = p->tickets;
      }
    }

    // SWITCHING TO CHOSEN PROC
    acquire(&chosenproc->lock);
    if (chosenproc->state == RUNNABLE)
    {
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      chosenproc->state = RUNNING;
      c->proc = chosenproc;
      swtch(&c->context, &chosenproc->context);

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&chosenproc->lock);
  }
#elif defined(PBS) // PRIORITY - BASED SCHEDULING (NON - PREEMPTIVE)
  printf("Scheduler : PBS\n");
  for (;;)
  {
    struct proc *chosenproc = proc;
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // FINDING CHOSEN PROCESS I.E PROCESS WITH MAXIMUM PRIORITY / LOWER DP VALUE 
    int min_dp = __INT32_MAX__;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE)
      {
        int dp = max(0, min(p->stpriority - p->niceness + 5, 100));
        if (dp < min_dp)
        {
          min_dp = dp;
          chosenproc = p;
        }
        else if (dp == min_dp)
        {
          if (p->numpicked < chosenproc->numpicked)
            chosenproc=p;
          else if(p->numpicked==chosenproc->numpicked)
          {
            if(p->starttime < chosenproc->starttime)
              chosenproc=p;
          }
        }
      }
    }

    // SWITCHING TO CHOSEN PROC
    acquire(&chosenproc->lock);
    if (chosenproc->state == RUNNABLE)
    {
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      chosenproc->state = RUNNING;
      chosenproc->numpicked++; // increment the number of times process is picked
      c->proc = chosenproc;
      swtch(&c->context, &chosenproc->context);

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&chosenproc->lock);
    chosenproc->niceness=(10*chosenproc->sleeptime)/(chosenproc->sleeptime + chosenproc->runtime);
  }
#elif defined(MLFQ)
  printf("Scheduler : MLFQ\n");
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // CHECK FOR RUNNABLE PROCESSES NOT IN ANY QUEUE 
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if ((p->state == RUNNABLE) && (p->qentertime==0))
        assignQueue(p);
    }

    // AGE THE PROCESSES
    for(int i=1;i<5;i++)
    {
      struct node* cur=queues[i];
      while(cur!=0)
      {
        struct proc* process=cur->p;
        if(1+ticks-process->qentertime>100) // 100 is the limit wait time
        {
          remove(i,process->pid); // remove the process from this queue
          process->queue=i-1;
          process->qentertime=ticks+1;
          push(i-1,process);
        }
        cur=cur->next;
      }
    }

    int q=0;
    while(q<5 && queues[q]==0){q++;} // finding first non-empty queue

    struct proc* chosenproc=pop(q);
    chosenproc->allowedtime = (1<<q);

    // SWITCHING TO CHOSEN PROC
    acquire(&chosenproc->lock);
    if (chosenproc->state == RUNNABLE)
    {
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      chosenproc->state = RUNNING;
      c->proc = chosenproc;
      swtch(&c->context, &chosenproc->context);

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&chosenproc->lock);

    chosenproc->qentertime=0;
    // if the process was preempted or came back from SLEEPING for IO
    if(chosenproc->state==RUNNABLE){
      if((chosenproc->allowedtime==0) && (q<4)){q++;} // move to higher queue if time is up
      chosenproc->queue=q;
      chosenproc->qentertime=ticks+1;
      push(q+1,chosenproc);
    }
  }
#endif
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        p->state = RUNNABLE;
        #ifdef MLFQ
          assignQueue(p);
        #endif 
      }
      release(&p->lock);
    }
  }
}

// update times of all process
void update_times() // called in clockintr when incrementing ticks
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);

    if (p->state == SLEEPING)
      p->sleeptime++;

    if (p->state == RUNNING)
      p->runtime++;

    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
        #ifdef MLFQ
          assignQueue(p);
        #endif 
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
