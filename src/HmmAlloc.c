/*
 * File : HmmAlloc.c
 * Desc : Heap Memory Manager allocation functions
 */

#include "internal.h"
#include "Hmm.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>


/*
 * Minimum amount requested from the OS.
 */
#define HMM_CHUNK_SIZE 4096U


/*
 * Head of the physical heap list.
 */
BlockHeader *heapHead = NULL;


/*
 * Head of the free list.
 */
BlockHeader *freeHead = NULL;
static char *heapFloor = NULL;

/*free functions
 * Add a block to the beginning of the free list.
 */
 void addToFreeList(BlockHeader *block)
{
    if (block == NULL)
    {
        return;
    }

    block->free = 1;

    block->prev_free = NULL;
    block->next_free = freeHead;

    if (freeHead != NULL)
    {
        freeHead->prev_free = block;
    }

    freeHead = block;
}


/*
 * Remove a block from the free list.
 */
static void removeFromFreeList(BlockHeader *block)
{
    if (block == NULL)
    {
        return;
    }

    if (block->prev_free != NULL)
    {
        block->prev_free->next_free = block->next_free;
    }
    else
    {
        /*
         * block is the first free block.
         */
        freeHead = block->next_free;
    }

    if (block->next_free != NULL)
    {
        block->next_free->prev_free = block->prev_free;
    }

    block->prev_free = NULL;
    block->next_free = NULL;

    block->free = 0;
}


/*
 * Find a free block large enough for size.
 *
 * First-fit strategy.
 */
static BlockHeader *findFreeBlock(size_t size)
{
    BlockHeader *current = freeHead;

    while (current != NULL)
    {
        if (current->size >= size)
        {
            return current;
        }

        current = current->next_free;
    }

    return NULL;
}


/* real list functions
*/


/*
 * Get the last physical block.
 */
static BlockHeader *getLastBlock(void)
{
    BlockHeader *current = heapHead;

    if (current == NULL)
    {
        return NULL;
    }

    while (current->next_phys != NULL)
    {
        current = current->next_phys;
    }

    return current;
}


/*
 * Add a newly created block to the physical list.
 */
static void addToPhysicalList(BlockHeader *block)
{
    BlockHeader *last;

    if (block == NULL)
    {
        return;
    }

    if (heapHead == NULL)
    {
        heapHead = block;
        return;
    }

    last = getLastBlock();

    last->next_phys = block;
    block->prev_phys = last;
}


/* 
 * REQUEST MEMORY FROM OS
 */


/*
 * Request a new chunk from the OS.
 *
 * Layout:
 *
 *      [BlockHeader][user memory]
 *
 * If the requested size is small,
 * request at least HMM_CHUNK_SIZE.
 */
static BlockHeader *requestChunk(size_t size)
{
    size_t totalSize;
    size_t chunkSize;
    void *memory;
    BlockHeader *block;

    /*
     * Protect:
     *
     * sizeof(BlockHeader) + size
     *
     * from overflowing size_t.
     */
    if (size > SIZE_MAX - sizeof(BlockHeader))
    {
        return NULL;
    }

    totalSize = sizeof(BlockHeader) + size;

    if (totalSize < HMM_CHUNK_SIZE)
    {
        chunkSize = HMM_CHUNK_SIZE;
    }
    else
    {
        chunkSize = totalSize;
    }

    /*
     * Ask the OS for more heap memory.
     *
     * sbrk() returns the OLD program break.
     */
    memory = sbrk((intptr_t)chunkSize);

    if (memory == (void *)-1)
    {
        return NULL;
    }
if (heapFloor == NULL)
{
    heapFloor = (char *)memory + chunkSize;
}

    block = (BlockHeader *)memory;

    /*
     * The first block owns the whole chunk initially.
     */
    block->size = chunkSize - sizeof(BlockHeader);
    block->free = 0;

    block->prev_phys = NULL;
    block->next_phys = NULL;

    block->prev_free = NULL;
    block->next_free = NULL;

    addToPhysicalList(block);

    return block;
}


/* =========================================================
 *                       SPLITTING
 * ========================================================= */



static void splitBlock(BlockHeader *block, size_t size)
{
    size_t remaining;
    BlockHeader *newBlock;

    if (block == NULL)
    {
        return;
    }

    if (block->size <= size)
    {
        return;
    }

    remaining = block->size - size;

    /*
     * Need enough space for:
     *
     *      new BlockHeader + at least 1 byte
     */
    if (remaining <= sizeof(BlockHeader))
    {
        return;
    }

    /*
     * The new header is placed immediately
     * after the requested user memory.
     */
    newBlock =
        (BlockHeader *)((char *)(block + 1) + size);

    newBlock->size =
        remaining - sizeof(BlockHeader);

    newBlock->free = 1;

    /*
     * Physical links.
     */
    newBlock->prev_phys = block;
    newBlock->next_phys = block->next_phys;

    if (block->next_phys != NULL)
    {
        block->next_phys->prev_phys = newBlock;
    }

    block->next_phys = newBlock;

    /*
     * The old block now represents
     * exactly the requested amount.
     */
    block->size = size;

    /*
     * The remainder becomes a free block.
     */
    newBlock->prev_free = NULL;
    newBlock->next_free = NULL;

    addToFreeList(newBlock);
}


/* =========================================================
 *                    COALESCING
 * ========================================================= */


/*
 * Merge block with the next physical block.
 *
 *      [block][next free]
 *
 * becomes:
 *
 *      [larger free block]
 */
 void mergeWithNext(BlockHeader *block)
{
    BlockHeader *next;

    if (block == NULL)
    {
        return;
    }

    next = block->next_phys;

    if (next == NULL || !next->free)
    {
        return;
    }

    /*
     * Remove next from the free list
     * because it is about to disappear.
     */
    removeFromFreeList(next);

    /*
     * The space consists of:
     *
     * block's old user space
     * + next header
     * + next user space
     */
    block->size += sizeof(BlockHeader) + next->size;

    /*
     * Skip next in the physical list.
     */
    block->next_phys = next->next_phys;

    if (next->next_phys != NULL)
    {
        next->next_phys->prev_phys = block;
    }
}


/*
 * Coalesce a free block with neighboring free blocks.
 */

   BlockHeader *coalesce(BlockHeader *block)
{
    if (block == NULL)
    {
        return NULL;
    }

    /* Merge with all free blocks before this block. */
    while (block->prev_phys != NULL &&
           block->prev_phys->free)
    {
        block = block->prev_phys;
        mergeWithNext(block);
    }

    /* Merge with all free blocks after this block. */
    while (block->next_phys != NULL &&
           block->next_phys->free)
    {
        mergeWithNext(block);
    }

    return block;
}
/* 
 *HEAP RELEASE
 */


/*
 * Release trailing free memory.
 *
 * If the last physical block is free,
 * we can move the program break backward.
 */
void releaseTrailingMemory(void)
{
    BlockHeader *last = getLastBlock();
    char *start, *end;

    if (last == NULL || !last->free)
    {
        return;
    }

    start = (char *)last;
    end   = (char *)(last + 1) + last->size;

    if (start >= heapFloor)
    {
        /* Whole block is above the floor: remove it. */
        BlockHeader *prev = last->prev_phys;   /* never NULL here */

        removeFromFreeList(last);
        prev->next_phys = NULL;

        if (sbrk(-(intptr_t)(end - start)) == (void *)-1)
        {
            /* sbrk failed: put the block back. */
            prev->next_phys = last;
            addToFreeList(last);
        }
    }
    else if (end > heapFloor &&
             (size_t)(heapFloor - start) > sizeof(BlockHeader))
    {
        /* Block crosses the floor: cut off only the part above it. */
        if (sbrk(-(intptr_t)(end - heapFloor)) != (void *)-1)
        {
            last->size = (size_t)(heapFloor - start) - sizeof(BlockHeader);
        }
    }
}
/*
 * Allocate memory.
 */
void *HmmAlloc(size_t size)
{
    BlockHeader *block;

    
        if (size == 0)
    {
        return NULL;
    }

    if (size > SIZE_MAX - 15U)
    {
        return NULL;
    }
    size = (size + 15U) & ~(size_t)15U;
    /*
     * Search existing free blocks first.
     */
    block = findFreeBlock(size);

    if (block != NULL)
    {
        /*
         * It is currently in the free list.
         */
        removeFromFreeList(block);

        /*
         * If it is larger than necessary,
         * split off the remainder.
         */
        splitBlock(block, size);
               return (void *)(block + 1);
    }

    /*
     * No suitable free block.
     * Request another chunk.
     */
    block = requestChunk(size);

    if (block == NULL)
    {
        return NULL;
    }

    /*
     * The chunk is probably larger than
     * the requested allocation.
     *
     * Split the extra memory into
     * a free block.
     */
    splitBlock(block, size);
          return (void *)(block + 1);
}


/* =========================================================
 *                      calloc
 * ========================================================= */


/*
 * Allocate nmemb elements of size bytes,
 * initialized to zero.
 */
void *hmmCalloc(size_t nmemb, size_t size)
{
    size_t total;
    void *ptr;

    /*
    
     *  overflowing.
     */
    if (size != 0 &&
        nmemb > SIZE_MAX / size)
    {
        return NULL;
    }

    total = nmemb * size;

    ptr = HmmAlloc(total);

    if (ptr == NULL)
    {
        return NULL;
    }

    memset(ptr, 0, total);

    return ptr;
}


/* =========================================================
 *                      realloc
 * ========================================================= */


/*
 * Reallocate memory.
 */
void *hmmReAlloc(void *ptr, size_t reqSize)
{
    BlockHeader *block;
    void *newPtr;
    size_t copySize;

    /*
     * realloc(NULL, size)
     * behaves like malloc(size).
     */
    if (ptr == NULL)
    {
        return HmmAlloc(reqSize);
    }

    /*
     * realloc(ptr, 0)
     *
     * We choose:
     *
     * free(ptr)
     * return NULL
     */
    if (reqSize == 0)
    {
        HmmFree(ptr);
        return NULL;
    }    if (reqSize > SIZE_MAX - 15U)
    {
        return NULL;
    }
    reqSize = (reqSize + 15U) & ~(size_t)15U;

    /*
     * Recover the header.
     */
    block = ((BlockHeader *)ptr) - 1;

    /*
     * Current block is already large enough.
     */
    if (block->size >= reqSize)
    {
        splitBlock(block, reqSize);

        return ptr;
    }

    /*
     * Try to expand into the next
     * physical free block.
     */
    if (block->next_phys != NULL &&
        block->next_phys->free)
    {
        BlockHeader *next = block->next_phys;

        if (block->size +
            sizeof(BlockHeader) +
            next->size >= reqSize)
        {
            removeFromFreeList(next);

            block->size +=
                sizeof(BlockHeader) + next->size;

            block->next_phys =
                next->next_phys;

            if (next->next_phys != NULL)
            {
                next->next_phys->prev_phys =
                    block;
            }

            splitBlock(block, reqSize);

            return ptr;
        }
    }

    /*
     * Cannot expand in place.
     * Allocate somewhere else.
     */
    newPtr = HmmAlloc(reqSize);

    if (newPtr == NULL)
    {
        return NULL;
    }

    /*
     * Only copy the amount that existed
     * in the old allocation.
     */
    copySize = block->size;

    if (copySize > reqSize)
    {
        copySize = reqSize;
    }

    memcpy(newPtr, ptr, copySize);

    HmmFree(ptr);

    return newPtr;
}


/* =========================================================
 *                       hmmSize
 * ========================================================= */


/*
 * Return user-visible size of allocation.
 */
size_t hmmSize(void *ptr)
{
    BlockHeader *block;

    if (ptr == NULL)
    {
        return 0U;
    }

    /*
     * ptr points immediately after the header.
     *
     * Move one BlockHeader backward.
     */
    block = ((BlockHeader *)ptr) - 1;

    return block->size;
}
