// samp OUT HZ prog args... : ptrace PC sampler. Lines: "S t_us tid rip orig_rax"; maps snapshots "M t_us" + maps text.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#define MAXT 4096
static int tids[MAXT], nt; static int pid; static FILE *o;
static long now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000+t.tv_nsec/1000;}
static void add(int t){for(int i=0;i<nt;i++)if(tids[i]==t)return; if(nt<MAXT)tids[nt++]=t;}
static void del(int t){for(int i=0;i<nt;i++)if(tids[i]==t){tids[i]=tids[--nt];return;}}
static char state(int t){char p[64],b[256];snprintf(p,64,"/proc/%d/stat",t);FILE*f=fopen(p,"r");if(!f)return 'X';int n=fread(b,1,255,f);fclose(f);b[n>0?n:0]=0;char*c=strrchr(b,')');return c?c[2]:'X';}
static int done;
static void handle(int t,int st,long t0,int want){ // returns via continuing
  if(WIFEXITED(st)||WIFSIGNALED(st)){del(t);if(t==pid)done=1;return;}
  if(!WIFSTOPPED(st))return;
  int ev=st>>16,sig=WSTOPSIG(st);
  if(ev==PTRACE_EVENT_STOP){ if(sig==SIGTRAP&&want){struct user_regs_struct r;if(!ptrace(PTRACE_GETREGS,t,0,&r))fprintf(o,"S %ld %d %llx %lld\n",now()-t0,t,r.rip,(long long)r.orig_rax);} ptrace(PTRACE_CONT,t,0,0);return;}
  if(ev){ if(ev==PTRACE_EVENT_CLONE||ev==PTRACE_EVENT_FORK||ev==PTRACE_EVENT_VFORK){unsigned long m;ptrace(PTRACE_GETEVENTMSG,t,0,&m);add((int)m);} ptrace(PTRACE_CONT,t,0,0);return;}
  ptrace(PTRACE_CONT,t,0,sig);
}
int main(int c,char**v){
  o=fopen(v[1],"w");int hz=atoi(v[2]);
  pid=fork();
  if(!pid){raise(SIGSTOP);execvp(v[3],v+3);_exit(127);}
  int st;waitpid(pid,&st,WUNTRACED);
  ptrace(PTRACE_SEIZE,pid,0,PTRACE_O_TRACECLONE|PTRACE_O_TRACEFORK|PTRACE_O_TRACEVFORK|PTRACE_O_TRACEEXEC|PTRACE_O_EXITKILL);
  kill(pid,SIGCONT);
  add(pid);long t0=now(),last=0;int tick=0;
  while(!done){
    while(1){int t=waitpid(-1,&st,__WALL|WNOHANG);if(t<=0)break;add(t);handle(t,st,t0,0);}
    if(done)break;
    struct timespec ts={0,1000000000L/hz};nanosleep(&ts,0);
    tick++;
    if(now()-last>50000){last=now();char p[64],b[65536];snprintf(p,64,"/proc/%d/maps",pid);FILE*f=fopen(p,"r");if(f){fprintf(o,"M %ld\n",now()-t0);int n;while((n=fread(b,1,sizeof b,f))>0)fwrite(b,1,n,o);fclose(f);fprintf(o,"E\n");}}
    for(int i=0;i<nt;i++){int t=tids[i];if(state(t)!='R')continue;
      if(ptrace(PTRACE_INTERRUPT,t,0,0))continue;
      int w=waitpid(t,&st,__WALL);if(w>0)handle(t,st,t0,1);
    }
  }
  fclose(o);return 0;
}
