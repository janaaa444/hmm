#ifndef HMM_INTERNAL_H
#define HMM_INTERNAL_H

#include <stddef.h>

typedef struct BlockHeader
{
    size_t size;
    int free;

    /* Physical list */
    struct BlockHeader *prev_phys;
    struct BlockHeader *next_phys;

    /* Free list */
    struct BlockHeader *prev_free;
    struct BlockHeader *next_free;

} BlockHeader;


void addToFreeList(BlockHeader *block);
BlockHeader *coalesce(BlockHeader *block);
void releaseTrailingMemory(void);
#endif
