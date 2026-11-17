#include "internal.h"
#include "Hmm.h"

void HmmFree(void *ptr)
{
    if (ptr == NULL)
        return;

    BlockHeader *block = ((BlockHeader *)ptr) - 1;

    block->free = 1;
    addToFreeList(block);

    block = coalesce(block);

    releaseTrailingMemory();
}
