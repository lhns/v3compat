// micro: effective-frequency, memory, syscall, fault and SMC costs, in ns (monotonic) and TSC.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <x86intrin.h>
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e9+t.tv_nsec;}
#define R(name,n,code) do{double b=1e18;uint64_t bt=0;for(int r=0;r<5;r++){double a=now();uint64_t x=__rdtsc();code;double e=now()-a;if(e<b){b=e;bt=__rdtsc()-x;}}printf("%-22s %9.2f ns %9.1f tsc\n",name,b/(n),(double)bt/(n));}while(0)
static void chase(size_t mb){size_t n=mb<<20,c=n/64;char*p=mmap(0,n,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);size_t*idx=malloc(c*sizeof *idx);for(size_t i=0;i<c;i++)idx[i]=i;srand(1);for(size_t i=c-1;i>0;i--){size_t j=((size_t)rand()*RAND_MAX+rand())%(i+1),t=idx[i];idx[i]=idx[j];idx[j]=t;}for(size_t i=0;i<c;i++)*(void**)(p+idx[i]*64)=p+idx[(i+1)%c]*64;void*v=p;const long N=2000000;char nm[32];sprintf(nm,"chase %zuMB",mb);R(nm,N,for(long i=0;i<N;i++)v=*(void**)v);if(!v)puts("");munmap(p,n);free(idx);}
int main(){
 const long N=200000000; volatile uint64_t s=0; uint64_t a0=1;
 R("alu chain(1 cycle ea)",N,for(long i=0;i<N;i++){a0+=i;__asm__ volatile("" : : : "memory");}); s=a0;
 chase(1);chase(32);chase(256);
 R("getppid",200000,for(int i=0;i<200000;i++)syscall(SYS_getppid));
 {char*m=mmap(0,1<<20,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);R("mprotect 4k pair",20000,for(int i=0;i<20000;i++){mprotect(m,4096,1);mprotect(m,4096,3);});}
 R("mmap+touch+unmap 1p",20000,for(int i=0;i<20000;i++){char*m=mmap(0,4096,3,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);*m=1;munmap(m,4096);});
 {size_t n=256<<20;R("minor fault",n/4096,char*m=mmap(0,n,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);for(size_t i=0;i<n;i+=4096)m[i]=1;munmap(m,n));}
 {unsigned char*c=mmap(0,8192,7,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);c[0]=0xc3;void(*f)(void)=(void*)c;const int K=2000000;
  R("call ret on rwx",K,for(int i=0;i<K;i++)f());
  R("patch+call same line",K,for(int i=0;i<K;i++){*(volatile unsigned char*)(c+8)=(unsigned char)i;f();});
  R("patch+call next page",K,for(int i=0;i<K;i++){*(volatile unsigned char*)(c+4096+8)=(unsigned char)i;f();});}
 {__m128i x=_mm_set1_epi32(1);R("avx-sse transition",1000000,for(int i=0;i<1000000;i++){__asm__ volatile("vxorps %%ymm1,%%ymm1,%%ymm1\n\tmovaps %%xmm2,%%xmm3\n\tvzeroupper" : : : "xmm1","xmm3");});(void)x;}
 printf("%llu\n",(unsigned long long)s);return 0;}
