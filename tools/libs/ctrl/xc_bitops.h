#ifndef XC_BITOPS_H
#define XC_BITOPS_H 1

/* bitmap operations for single threaded access */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef __LP64__
#define BITS_PER_LONG 64
#else
#define BITS_PER_LONG 32
#endif

#define BITMAP_ENTRY(_nr,_bmap) ((_bmap))[(_nr) / 8]
#define BITMAP_SHIFT(_nr) ((_nr) % 8)

/* calculate required space for number of bytes needed to hold nr_bits */
static inline unsigned long bitmap_size(unsigned long nr_bits)
{
    return (nr_bits + 7) / 8;
}

static inline void *bitmap_alloc(unsigned long nr_bits)
{
    unsigned long longs;

    longs = (nr_bits + BITS_PER_LONG - 1) / BITS_PER_LONG;
    return calloc(longs, sizeof(unsigned long));
}

static inline void bitmap_set(void *addr, unsigned long nr_bits)
{
    memset(addr, 0xff, bitmap_size(nr_bits));
}

static inline void bitmap_clear(void *addr, unsigned long nr_bits)
{
    memset(addr, 0, bitmap_size(nr_bits));
}

static inline int test_bit(unsigned long nr, const void *_addr)
{
    const char *addr = _addr;
    return (BITMAP_ENTRY(nr, addr) >> BITMAP_SHIFT(nr)) & 1;
}

static inline void clear_bit(unsigned long nr, void *_addr)
{
    char *addr = _addr;
    BITMAP_ENTRY(nr, addr) &= ~(1UL << BITMAP_SHIFT(nr));
}

static inline void set_bit(unsigned long nr, void *_addr)
{
    char *addr = _addr;
    BITMAP_ENTRY(nr, addr) |= (1UL << BITMAP_SHIFT(nr));
}

static inline int test_and_clear_bit(unsigned long nr, void *addr)
{
    int oldbit = test_bit(nr, addr);
    clear_bit(nr, addr);
    return oldbit;
}

static inline int test_and_set_bit(unsigned long nr, void *addr)
{
    int oldbit = test_bit(nr, addr);
    set_bit(nr, addr);
    return oldbit;
}

static inline void bitmap_or(void *_dst, const void *_other,
                             unsigned long nr_bits)
{
    char *dst = _dst;
    const char *other = _other;
    unsigned long i;
    for ( i = 0; i < bitmap_size(nr_bits); ++i )
        dst[i] |= other[i];
}

static inline bool test_bit_long_set(unsigned long nr_base, const void *_addr)
{
    const unsigned long *addr = _addr;
    unsigned long val = addr[nr_base / BITS_PER_LONG];

    return val == ~0;
}

static inline bool test_bit_long_clear(unsigned long nr_base, const void *_addr)
{
    const unsigned long *addr = _addr;
    unsigned long val = addr[nr_base / BITS_PER_LONG];

    return val == 0;
}

static inline void clear_bit_long(unsigned long nr_base, void *_addr)
{
    unsigned long *addr = _addr;
    addr[nr_base / BITS_PER_LONG] = 0;
}

static inline void set_bit_long(unsigned long nr_base, void *_addr)
{
    unsigned long *addr = _addr;
    addr[nr_base / BITS_PER_LONG] = ~0;
}
#endif  /* XC_BITOPS_H */
