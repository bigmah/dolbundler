/* isolate the cost of the big-endian guest load: bswap has an instruction on
   arm64 (rev) and none in wasm. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double sec(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static uint8_t ram[24u<<20];
__attribute__((noinline)) static uint32_t raw_sum(uint32_t n){
    uint32_t s=0; for(uint32_t i=0;i<n;i++){ uint32_t v; __builtin_memcpy(&v,ram+((i*4u)&0xFFFFFCu),4); s+=v; } return s; }
__attribute__((noinline)) static uint32_t swap_sum(uint32_t n){
    uint32_t s=0; for(uint32_t i=0;i<n;i++){ uint32_t v; __builtin_memcpy(&v,ram+((i*4u)&0xFFFFFCu),4); s+=__builtin_bswap32(v); } return s; }
int main(int c,char**v){
    uint32_t n = c>1?(uint32_t)strtoul(v[1],0,0):50000000u;
    for(size_t i=0;i<sizeof ram;i++) ram[i]=(uint8_t)i;
    double b1=1e30,b2=1e30; uint32_t s1=0,s2=0;
    for(int r=0;r<5;r++){ double t=sec(); s1=raw_sum(n);  double d=sec()-t; if(d<b1)b1=d; }
    for(int r=0;r<5;r++){ double t=sec(); s2=swap_sum(n); double d=sec()-t; if(d<b2)b2=d; }
    printf("raw  %.3f ns/load\nswap %.3f ns/load\noverhead %.3f ns  (%.2fx)\nchk %u %u\n",
           b1*1e9/n, b2*1e9/n, (b2-b1)*1e9/n, b2/b1, s1, s2);
    return 0; }
