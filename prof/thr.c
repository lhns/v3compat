#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
static void *f(void *a){return a;}
int main(int c,char**v){int n=atoi(v[1]);pthread_t t[n];for(int i=0;i<n;i++)pthread_create(&t[i],0,f,0);for(int i=0;i<n;i++)pthread_join(t[i],0);return 0;}
