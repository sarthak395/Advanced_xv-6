#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_trace(void) // JUST FOR CALLING SOME SYSTEM CALL IN PROC.C 
{
  // The functions in sysproc.c can access the process structure of a given process by calling myproc()
  int mask;
  argint(0, &mask);
  myproc()->mask=mask;
  return 1; // during process initialisation only , we updated value of mask
}

uint64
sys_sigalarm(void) // JUST FOR CALLING SOME SYSTEM CALL IN PROC.C 
{
  // The functions in sysproc.c can access the process structure of a given process by calling myproc()
  int n;
  uint64 handler;

  argint(0,&n);
  argaddr(1,&handler);

  myproc()->is_sigalarm=0;
  myproc()->tslalarm=0;
  myproc()->alarmint=n;
  myproc()->alarmhandler=handler;

  // just alert the user every n ticks 
  return 1; // during process initialisation only , we updated value of mask
}

uint64 sys_sigreturn(void){
  struct proc* p=myproc();

  // Restoring kernel stack for trapframe
  p->tf_copy->kernel_satp=p->trapframe->kernel_satp;
  p->tf_copy->kernel_sp=p->trapframe->kernel_sp;
  p->tf_copy->kernel_trap=p->trapframe->kernel_trap;
  p->tf_copy->kernel_hartid=p->trapframe->kernel_hartid;

  // restoring previous things of trapframe
  *(p->trapframe)=*(p->tf_copy); 
  p->is_sigalarm=0; // disabling alarm
  return 1;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
