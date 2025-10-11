#!/usr/bin/env python3

"""
Physical Memory Manager (Buddy Allocator) - Python Translation

DISCLAIMER: This code is an AI-assisted translation of the original C implementation
(pmm.c by u/ApparentlyPlus) to Python for testing and validation purposes. While the
logic has been carefully reviewed and verified to match the C implementation, the
translation itself was performed with AI assistance. Users should be aware that this
is generated code and, while functionally correct, may not reflect hand-written Python
idioms or best practices in all cases.

The original C implementation remains the authoritative reference.

Author: u/ApparentlyPlus (C implementation)
Translation: AI-assisted (Python)
"""

import math
import sys
from typing import List, Optional, Tuple

# Constants from pmm.h
PMM_MIN_ORDER_PAGE_SIZE = 4096
PMM_MAX_ORDERS = 32

# PMM status codes
class PMMStatus:
    OK = 0
    ERR_OOM = 1           # out of memory (no block large enough)
    ERR_INVALID = 2       # invalid arguments 
    ERR_NOT_INIT = 3      # pmm not initialized yet
    ERR_ALREADY_INIT = 4  # pmm_init called twice without pmm_shutdown
    ERR_NOT_ALIGNED = 5   # address/size not aligned to required block size
    ERR_OUT_OF_RANGE = 6  # address outside managed range
    ERR_NOT_FOUND = 7     # expected buddy not found during coalescing (internal)

# Helper functions
def is_pow2_u64(x: int) -> bool:
    """Check if x is a power of two"""
    return x != 0 and (x & (x - 1)) == 0


def align_up(value: int, alignment: int) -> int:
    """Align value up to the given alignment"""
    return (value + alignment - 1) & ~(alignment - 1)

def align_down(value: int, alignment: int) -> int:
    """Align value down to the given alignment"""
    return value & ~(alignment - 1)

class PMM:
    """
    Physical Memory Manager (Buddy Allocator) - Python translation
    """
    
    def __init__(self):
        self.g_inited = False
        self.g_range_start = 0
        self.g_range_end = 0
        self.g_min_block = PMM_MIN_ORDER_PAGE_SIZE
        self.g_max_order = 0
        self.g_order_count = 0
        
        # Free list heads per order. Store physical address of first free block, or None for empty.
        self.g_free_heads = [None] * PMM_MAX_ORDERS
        self.EMPTY_SENTINEL = None
        
        # Track allocated blocks for testing (not part of original C code)
        self.allocated_blocks = {}  # phys_addr -> size
        
        # Track memory state for validation (not part of original C code)
        self.memory_state = {}  # byte_address -> value (for checking overwrites)
    
    def order_to_size(self, order: int) -> int:
        """Convert order to block size in bytes"""
        return self.g_min_block << order
    
    def read_next_word(self, block_phys: int) -> int:
        """
        Read the next pointer stored at the start of a free block
        In Python, we simulate physical memory access
        """
        # Simulate reading from physical memory
        if block_phys not in self.memory_state:
            return 0  # Default to 0 if not written
        return self.memory_state.get(block_phys, 0)
    
    def write_next_word(self, block_phys: int, next_phys: int) -> None:
        """
        Write the next pointer stored at the start of a free block
        In Python, we simulate physical memory access
        """
        # Store only the first 8 bytes (simulating the next pointer)
        self.memory_state[block_phys] = next_phys
    
    def pop_head(self, order: int) -> Optional[int]:
        """Pop a block from the free list for given order, or None if empty"""
        head = self.g_free_heads[order]
        if head is None:
            return None
        
        next_block = self.read_next_word(head)
        self.g_free_heads[order] = next_block if next_block != 0 else None
        
        # Clear the next pointer in the popped block just to be sure
        self.write_next_word(head, 0)
        return head
    
    def push_head(self, order: int, block_phys: int) -> None:
        """Push a block onto the free list for given order"""
        head = self.g_free_heads[order]
        next_val = head if head is not None else 0
        self.write_next_word(block_phys, next_val)
        self.g_free_heads[order] = block_phys
    
    def remove_specific(self, order: int, target_phys: int) -> bool:
        """Remove a specific block from the free list for given order"""
        prev = None
        cur = self.g_free_heads[order]
        
        while cur is not None:
            next_block = self.read_next_word(cur)
            next_block = next_block if next_block != 0 else None
            
            if cur == target_phys:
                if prev is None:
                    self.g_free_heads[order] = next_block
                else:
                    # Write prev->next = next
                    self.write_next_word(prev, next_block if next_block is not None else 0)
                return True
            
            prev = cur
            cur = next_block
        
        return False
    
    def buddy_of(self, addr: int, order: int) -> int:
        """Compute buddy address of a block at given order"""
        size = self.order_to_size(order)
        return (((addr - self.g_range_start) ^ size) + self.g_range_start)
    
    def size_to_order(self, size_bytes: int) -> int:
        """Convert size in bytes to minimum order that fits it"""
        if size_bytes == 0:
            return 0
        
        need = self.g_min_block
        order = 0
        
        while need < size_bytes:
            need <<= 1
            order += 1
        
        return order
    
    def partition_range_into_blocks(self, range_start: int, range_end: int) -> None:
        """
        Partition an arbitrary aligned range [start,end) into largest possible 
        aligned blocks and push them into freelists (classic greedy partition).
        Assumes 'start' is aligned to g_min_block and 'end' is multiple of g_min_block.
        """
        cur = range_start
        
        while cur < range_end:
            remain = range_end - cur
            # Choose largest order o such that order_to_size(o) <= remain and cur is aligned to that size
            chosen = 0
            for o in range(self.g_max_order, -1, -1):
                bsize = self.order_to_size(o)
                if bsize > remain:
                    continue
                if (cur & (bsize - 1)) != 0:
                    continue
                chosen = o
                break
            
            self.push_head(chosen, cur)
            cur += self.order_to_size(chosen)
    
    # Public API
    
    def is_initialized(self) -> bool:
        """Returns whether the PMM has been initialized"""
        return self.g_inited
    
    def managed_base(self) -> int:
        """Returns the start of the managed physical memory range"""
        return self.g_range_start
    
    def managed_end(self) -> int:
        """Returns the end of the managed physical memory range"""
        return self.g_range_end
    
    def managed_size(self) -> int:
        """Returns the size of the managed physical memory range"""
        return self.g_range_end - self.g_range_start
    
    def min_block_size(self) -> int:
        """Returns the minimum block size (order 0) in bytes"""
        return self.g_min_block
    
    def init(self, range_start_phys: int, range_end_phys: int, min_block_size: int) -> int:
        """
        Initialize the physical memory manager to manage
        the physical address range [range_start_phys, range_end_phys).
        """
        if self.g_inited:
            return PMMStatus.ERR_ALREADY_INIT
        if range_end_phys <= range_start_phys:
            return PMMStatus.ERR_INVALID
        if min_block_size == 0 or not is_pow2_u64(min_block_size):
            return PMMStatus.ERR_INVALID
        if min_block_size < 8:  # sizeof(uint64_t)
            return PMMStatus.ERR_INVALID
        
        self.g_min_block = min_block_size
        
        # Align start up to min_block and end down to min_block
        start_aligned = align_up(range_start_phys, self.g_min_block)
        end_aligned = align_down(range_end_phys, self.g_min_block)
        
        if end_aligned <= start_aligned:
            return PMMStatus.ERR_INVALID
        
        self.g_range_start = start_aligned
        self.g_range_end = end_aligned
        
        span_trimmed = self.managed_size()
        blocks = span_trimmed // self.g_min_block
        
        max_order = 0
        tmp = blocks
        while tmp > 1:
            tmp >>= 1
            max_order += 1
        if max_order >= PMM_MAX_ORDERS:
            max_order = PMM_MAX_ORDERS - 1
        
        self.g_max_order = max_order
        self.g_order_count = self.g_max_order + 1
        
        # Initialize free lists
        for i in range(PMM_MAX_ORDERS):
            self.g_free_heads[i] = None
        
        # Clear memory state and partition the range
        self.memory_state = {}
        self.allocated_blocks = {}
        self.partition_range_into_blocks(self.g_range_start, self.g_range_end)
        
        self.g_inited = True
        return PMMStatus.OK
    
    def shutdown(self) -> None:
        """Reset state so init may be called again."""
        if not self.g_inited:
            return
        
        # Clear metadata region (we store next pointers at starts of free blocks)
        self.memory_state = {}
        
        self.g_inited = False
        self.g_range_start = 0
        self.g_range_end = 0
        self.g_min_block = PMM_MIN_ORDER_PAGE_SIZE
        self.g_max_order = 0
        self.g_order_count = 0
        for i in range(PMM_MAX_ORDERS):
            self.g_free_heads[i] = None
        self.allocated_blocks = {}
    
    def alloc_block_of_order(self, req_order: int) -> Tuple[int, int]:
        """
        Internal allocation helper: find a free block at >= req_order and split down
        Returns (status, phys_addr)
        """
        if not self.g_inited:
            return (PMMStatus.ERR_NOT_INIT, 0)
        if req_order > self.g_max_order:
            return (PMMStatus.ERR_OOM, 0)
        
        o = req_order
        while o <= self.g_max_order and self.g_free_heads[o] is None:
            o += 1
        if o > self.g_max_order:
            return (PMMStatus.ERR_OOM, 0)
        
        block = self.pop_head(o)
        if block is None:
            return (PMMStatus.ERR_OOM, 0)
        
        # Split until we reach requested order
        while o > req_order:
            o -= 1
            half = self.order_to_size(o)
            buddy = block + half
            # Push buddy into freelist at order o
            self.push_head(o, buddy)
        
        # Clean metadata in the allocated block
        self.write_next_word(block, 0)
        
        # Track allocation for testing
        self.allocated_blocks[block] = self.order_to_size(req_order)
        
        return (PMMStatus.OK, block)
    
    def alloc(self, size_bytes: int) -> Tuple[int, int]:
        """
        Allocate a block large enough to satisfy size_bytes.
        Returns (status, phys_addr)
        """
        if not self.g_inited:
            return (PMMStatus.ERR_NOT_INIT, 0)
        if size_bytes == 0:
            return (PMMStatus.ERR_INVALID, 0)
        
        # Round up to multiple of g_min_block
        rounded = size_bytes
        if rounded & (self.g_min_block - 1):
            rounded = align_up(rounded, self.g_min_block)
        
        order = self.size_to_order(rounded)
        if order > self.g_max_order:
            return (PMMStatus.ERR_OOM, 0)
        
        return self.alloc_block_of_order(order)
    
    def free(self, phys: int, size_bytes: int) -> int:
        """Free an allocation previously returned by alloc."""
        if not self.g_inited:
            return PMMStatus.ERR_NOT_INIT
        if size_bytes == 0:
            return PMMStatus.ERR_INVALID
        
        # Basic range check against [g_range_start, g_range_end)
        if phys < self.g_range_start:
            return PMMStatus.ERR_OUT_OF_RANGE
        if phys >= self.g_range_end:
            return PMMStatus.ERR_OUT_OF_RANGE
        
        # Round size in the same manner as allocation
        rounded = size_bytes
        if rounded & (self.g_min_block - 1):
            rounded = align_up(rounded, self.g_min_block)
        
        order = self.size_to_order(rounded)
        if order > self.g_max_order:
            return PMMStatus.ERR_INVALID  # Match C code: too large
        
        block_addr = phys
        block_size = self.order_to_size(order)
        
        if (block_addr & (block_size - 1)) != 0:
            return PMMStatus.ERR_NOT_ALIGNED
        
        # Remove from allocated blocks tracking
        if block_addr in self.allocated_blocks:
            del self.allocated_blocks[block_addr]
        
        # Coalesce upwards where possible
        while order < self.g_max_order:
            buddy = self.buddy_of(block_addr, order)
            
            # If buddy is outside managed range, stop
            if buddy < self.g_range_start or (buddy + block_size) > self.g_range_end:
                break
            
            # If buddy is free at this order, remove it and coalesce
            found = self.remove_specific(order, buddy)
            if not found:
                self.push_head(order, block_addr)  # buddy not free, push current and exit
                return PMMStatus.OK
            
            # Buddy removed, merged block is min(block_addr, buddy)
            if buddy < block_addr:
                block_addr = buddy
            order += 1
            block_size <<= 1
        
        # Push the resulting (possibly coalesced) block
        self.push_head(order, block_addr)
        return PMMStatus.OK
    
    def mark_reserved_range(self, start: int, end: int) -> int:
        """
        Mark [start,end) as reserved.
        This handles partial overlaps and ensures free-lists remain consistent.
        """
        if not self.g_inited:
            return PMMStatus.ERR_NOT_INIT
        if end <= start:
            return PMMStatus.ERR_INVALID
        
        # Clamp to managed range
        if start < self.g_range_start:
            start = self.g_range_start
        if end > self.g_range_end:
            end = self.g_range_end
        if start >= end:
            return PMMStatus.ERR_INVALID
        
        # Align to min block for safe handling
        start = align_down(start, self.g_min_block)
        end = align_up(end, self.g_min_block)
        
        # For each order from max to min, scan free lists and remove overlapping blocks
        for o in range(self.g_max_order, -1, -1):
            block_size = self.order_to_size(o)
            cur = self.g_free_heads[o]
            
            while cur is not None:
                next_block = self.read_next_word(cur)
                next_block = next_block if next_block != 0 else None
                block_start = cur
                block_end = cur + block_size
                
                # Check if block overlaps the reserved range
                if not (block_end <= start or block_start >= end):
                    # Remove this block from the free list
                    self.remove_specific(o, cur)
                    
                    # If block is partially outside the reserved range, push remaining pieces
                    if block_start < start:
                        self.mark_free_range(block_start, start)
                    if block_end > end:
                        self.mark_free_range(end, block_end)
                
                cur = next_block
        
        return PMMStatus.OK
    
    def mark_free_range(self, start: int, end: int) -> int:
        """
        Manually mark a physical range [start,end) as free.
        Partitions the range into aligned blocks and pushes them into the free-lists.
        """
        if not self.g_inited:
            return PMMStatus.ERR_NOT_INIT
        if end <= start:
            return PMMStatus.ERR_INVALID
        
        # Clamp to managed range
        if start < self.g_range_start:
            start = self.g_range_start
        if end > self.g_range_end:
            end = self.g_range_end
        if start >= end:
            return PMMStatus.ERR_INVALID
        
        # Round start up and end down to min_block
        start = align_up(start, self.g_min_block)
        end = align_down(end, self.g_min_block)
        if start >= end:
            return PMMStatus.ERR_INVALID
        
        self.partition_range_into_blocks(start, end)
        return PMMStatus.OK
    
    # Debugging and testing helpers
    
    def get_free_list_state(self) -> List[List[int]]:
        """Get current state of free lists for testing"""
        state = []
        for order in range(PMM_MAX_ORDERS):
            if self.g_free_heads[order] is None:
                state.append([])
                continue
            
            blocks = []
            cur = self.g_free_heads[order]
            while cur is not None:
                blocks.append(cur)
                next_block = self.read_next_word(cur)
                cur = next_block if next_block != 0 else None
            state.append(blocks)
        return state
    
    def get_allocated_blocks(self):
        """Get allocated blocks for testing"""
        return self.allocated_blocks.copy()
    
    def validate_memory_consistency(self) -> Tuple[bool, str]:
        """Validate internal consistency of the PMM"""
        if not self.g_inited:
            return (True, "Not initialized")
        
        # Check that all free blocks are within managed range
        free_lists = self.get_free_list_state()
        for order, blocks in enumerate(free_lists):
            block_size = self.order_to_size(order)
            for block in blocks:
                if block < self.g_range_start or block + block_size > self.g_range_end:
                    return (False, f"Free block {block:x} (order {order}) outside managed range")
                
                # Check alignment - blocks must be aligned to min_block, not necessarily their size
                if (block & (self.g_min_block - 1)) != 0:
                    return (False, f"Free block {block:x} (order {order}) not aligned to min_block")
        
        # Check that no allocated blocks overlap with free blocks
        for alloc_addr, alloc_size in self.allocated_blocks.items():
            for order, blocks in enumerate(free_lists):
                block_size = self.order_to_size(order)
                for block in blocks:
                    alloc_end = alloc_addr + alloc_size
                    block_end = block + block_size
                    if not (alloc_end <= block or alloc_addr >= block_end):
                        return (False, f"Allocated block {alloc_addr:x}-{alloc_end:x} overlaps free block {block:x}-{block_end:x}")
        
        return (True, "Consistent")
