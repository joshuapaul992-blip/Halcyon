#ifndef IO_H
#define IO_H
#include "types.h"

static inline void outb(u16 port, u8 v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }
static inline u8  inb(u16 port) { u8 v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

/* MMIO accessors */
static inline u8  mmio_read8(volatile void *a)  { return *(volatile u8 *)a; }
static inline u16 mmio_read16(volatile void *a) { return *(volatile u16 *)a; }
static inline u32 mmio_read32(volatile void *a) { return *(volatile u32 *)a; }
static inline u64 mmio_read64(volatile void *a) {
    /* split into two 32-bit reads: some controllers do not support 64-bit MMIO */
    u32 lo = *(volatile u32 *)a;
    u32 hi = *((volatile u32 *)a + 1);
    return ((u64)hi << 32) | lo;
}
static inline void mmio_write8(volatile void *a, u8 v)   { *(volatile u8 *)a = v; }
static inline void mmio_write16(volatile void *a, u16 v) { *(volatile u16 *)a = v; }
static inline void mmio_write32(volatile void *a, u32 v) { *(volatile u32 *)a = v; }
static inline void mmio_write64(volatile void *a, u64 v) {
    *(volatile u32 *)a = (u32)v;
    *((volatile u32 *)a + 1) = (u32)(v >> 32);
}

static inline void mb(void)  { __asm__ volatile("mfence" ::: "memory"); }
static inline void rmb(void) { __asm__ volatile("lfence" ::: "memory"); }
static inline void wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
static inline void barrier(void) { __asm__ volatile("" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }

static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 v) {
    __asm__ volatile("wrmsr" :: "a"((u32)v), "d"((u32)(v >> 32)), "c"(msr));
}
static inline void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

#endif
