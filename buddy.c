#include "buddy.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define PAGE_SIZE (4 * 1024)
#define MAX_RANK 16

typedef struct free_block {
    struct free_block *next;
} free_block_t;

static free_block_t *free_lists[MAX_RANK + 1];
static void *base_addr = NULL;
static int total_pages = 0;
static uint8_t *rank_map = NULL;
static bool *allocated_map = NULL;

// Help function to get rank of a block from its index
static int get_block_index(void *p) {
    return ((uintptr_t)p - (uintptr_t)base_addr) / PAGE_SIZE;
}

static void add_to_list(int rank, void *p) {
    free_block_t *block = (free_block_t *)p;
    block->next = free_lists[rank];
    free_lists[rank] = block;
}

static void remove_from_list(int rank, void *p) {
    free_block_t **curr = &free_lists[rank];
    while (*curr) {
        if (*curr == (free_block_t *)p) {
            *curr = (*curr)->next;
            return;
        }
        curr = &((*curr)->next);
    }
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0) return -EINVAL;
    
    base_addr = p;
    total_pages = pgcount;
    
    for (int i = 0; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    rank_map = (uint8_t *)malloc(pgcount * sizeof(uint8_t));
    allocated_map = (bool *)calloc(pgcount, sizeof(bool));
    if (!rank_map || !allocated_map) return -EINVAL;
    
    memset(rank_map, 0, pgcount * sizeof(uint8_t));

    // Split pgcount into biggest possible power-of-2 blocks
    int current_offset = 0;
    while (current_offset < pgcount) {
        int max_rank = MAX_RANK;
        while (max_rank >= 1) {
            int size = (1 << (max_rank - 1));
            if (current_offset + size <= pgcount && 
                (current_offset % size == 0)) {
                break;
            }
            max_rank--;
        }
        
        if (max_rank < 1) break; // Should not happen given constraints

        void *block_start = (char *)base_addr + (uintptr_t)current_offset * PAGE_SIZE;
        add_to_list(max_rank, block_start);
        
        // Mark the rank for query_ranks. The first page of the block stores the rank.
        rank_map[current_offset] = max_rank;
        
        current_offset += (1 << (max_rank - 1));
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) return ERR_PTR(-EINVAL);

    int search_rank = rank;
    while (search_rank <= MAX_RANK && !free_lists[search_rank]) {
        search_rank++;
    }

    if (search_rank > MAX_RANK) return ERR_PTR(-ENOSPC);

    // Get a block from search_rank and split it until we reach rank
    void *block = free_lists[search_rank];
    remove_from_list(search_rank, block);

    while (search_rank > rank) {
        search_rank--;
        int size = (1 << (search_rank - 1));
        void *buddy = (char *)block + (uintptr_t)size * PAGE_SIZE;
        add_to_list(search_rank, buddy);
        // We don't mark rank_map for free blocks here because the buddy system
        // usually cares about the rank of a block when it's allocated or for queries.
        // But rank_map should represent the current state of the memory.
        // Let's manage rank_map consistently.
    }

    int idx = get_block_index(block);
    rank_map[idx] = rank;
    allocated_map[idx] = true;

    return block;
}

int return_pages(void *p) {
    if (!p) return -EINVAL;
    if ((uintptr_t)p < (uintptr_t)base_addr || (uintptr_t)p >= (uintptr_t)base_addr + (uintptr_t)total_pages * PAGE_SIZE) return -EINVAL;
    
    int idx = get_block_index(p);
    if (idx < 0 || idx >= total_pages || !allocated_map[idx]) return -EINVAL;

    int rank = rank_map[idx];
    allocated_map[idx] = false;

    void *curr_p = p;
    int curr_rank = rank;

    while (curr_rank < MAX_RANK) {
        int size = (1 << (curr_rank - 1));
        int curr_idx = get_block_index(curr_p);
        int buddy_idx = curr_idx ^ size;

        if (buddy_idx < 0 || buddy_idx >= total_pages) break;

        void *buddy_p = (char *)base_addr + (uintptr_t)buddy_idx * PAGE_SIZE;

        // Buddy must be free and have the same rank
        if (allocated_map[buddy_idx] || rank_map[buddy_idx] != curr_rank) break;



        remove_from_list(curr_rank, buddy_p);
        
        // Combine
        if (buddy_idx < curr_idx) {
            curr_p = buddy_p;
            curr_idx = buddy_idx;
        }
        curr_rank++;
        rank_map[curr_idx] = curr_rank;
    }

    add_to_list(curr_rank, curr_p);
    rank_map[get_block_index(curr_p)] = curr_rank;

    return OK;
}

int query_ranks(void *p) {
    if (!p) return -EINVAL;
    if ((uintptr_t)p < (uintptr_t)base_addr || (uintptr_t)p >= (uintptr_t)base_addr + (uintptr_t)total_pages * PAGE_SIZE) return -EINVAL;

    int idx = get_block_index(p);
    if (idx < 0 || idx >= total_pages) return -EINVAL;

    if (allocated_map[idx]) return rank_map[idx];

    for (int r = MAX_RANK; r >= 1; r--) {
        free_block_t *curr = free_lists[r];
        while (curr) {
            int b_idx = get_block_index(curr);
            int size = (1 << (r - 1));
            if (idx >= b_idx && idx < b_idx + size) {
                return r;
            }
            curr = curr->next;
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;
    
    int count = 0;
    free_block_t *curr = free_lists[rank];
    while (curr) {
        count++;
        curr = curr->next;
    }
    return count;
}
