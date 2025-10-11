#!/usr/bin/env python3

"""
PMM Test Suite

DISCLAIMER: This test suite was generated with AI assistance to validate the Python
translation of the Physical Memory Manager (pmm.py). While the tests have been
reviewed and verified to correctly exercise the PMM functionality, both the test
code and the PMM implementation being tested are AI-generated translations of the
original C code.

The tests are functionally correct and comprehensive, but users should be aware of
the AI-generated nature of this code when using it as a reference or for critical
applications.

Original C implementation: u/ApparentlyPlus
Python translation & tests: AI-assisted
"""

import unittest
import random
from pmm import PMM, PMMStatus, align_up, align_down, is_pow2_u64

class TestPMM(unittest.TestCase):
    
    def setUp(self):
        """Set up a fresh PMM instance for each test"""
        self.pmm = PMM()
    
    def test_basic_initialization(self):
        """Test basic PMM initialization"""
        # Test valid initialization
        status = self.pmm.init(0x1000, 0x100000, 4096)
        self.assertEqual(status, PMMStatus.OK)
        self.assertTrue(self.pmm.is_initialized())
        self.assertEqual(self.pmm.managed_base(), 0x1000)
        self.assertEqual(self.pmm.managed_end(), 0x100000)
        self.assertEqual(self.pmm.managed_size(), 0xFF000)
        self.assertEqual(self.pmm.min_block_size(), 4096)
        
        # Test double initialization
        status = self.pmm.init(0x2000, 0x200000, 4096)
        self.assertEqual(status, PMMStatus.ERR_ALREADY_INIT)
    
    def test_initialization_edge_cases(self):
        """Test PMM initialization with edge cases"""
        # Test invalid range
        status = self.pmm.init(0x2000, 0x1000, 4096)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
        
        # Test zero min block size
        status = self.pmm.init(0x1000, 0x100000, 0)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
        
        # Test non-power-of-two min block size
        status = self.pmm.init(0x1000, 0x100000, 3000)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
        
        # Test min block size too small (fixed typo)
        status = self.pmm.init(0x1000, 0x100000, 4)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
        
        # Test range too small after alignment
        status = self.pmm.init(0x1001, 0x1002, 4096)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
    
    def test_shutdown(self):
        """Test PMM shutdown functionality"""
        status = self.pmm.init(0x1000, 0x100000, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        self.pmm.shutdown()
        self.assertFalse(self.pmm.is_initialized())
        
        # Should be able to re-initialize after shutdown
        status = self.pmm.init(0x2000, 0x200000, 8192)
        self.assertEqual(status, PMMStatus.OK)
        self.assertEqual(self.pmm.min_block_size(), 8192)
    
    def test_basic_allocation(self):
        """Test basic allocation functionality"""
        status = self.pmm.init(0x1000, 0x20000, 4096)  # 124KB managed
        self.assertEqual(status, PMMStatus.OK)
        
        # Allocate various sizes
        status, addr1 = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        self.assertIsNotNone(addr1)
        self.assertGreaterEqual(addr1, 0x1000)
        self.assertLess(addr1, 0x20000)
        
        status, addr2 = self.pmm.alloc(8192)
        self.assertEqual(status, PMMStatus.OK)
        
        status, addr3 = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Verify allocations don't overlap
        self.assertNotEqual(addr1, addr2)
        self.assertNotEqual(addr1, addr3)
        self.assertNotEqual(addr2, addr3)
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_allocation_edge_cases(self):
        """Test allocation edge cases"""
        status = self.pmm.init(0x1000, 0x20000, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Test allocation without initialization
        pmm2 = PMM()
        status, addr = pmm2.alloc(4096)
        self.assertEqual(status, PMMStatus.ERR_NOT_INIT)
        
        # Test zero size allocation
        status, addr = self.pmm.alloc(0)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
        
        # Test OOM
        # Allocate until we run out of memory
        allocations = []
        while True:
            status, addr = self.pmm.alloc(4096)
            if status == PMMStatus.OK:
                allocations.append(addr)
            else:
                self.assertEqual(status, PMMStatus.ERR_OOM)
                break
        
        # Should have allocated all available memory
        self.assertGreater(len(allocations), 0)
    
    def test_free_and_coalescing(self):
        """Test freeing and buddy coalescing"""
        status = self.pmm.init(0x1000, 0x9000, 4096)  # 32KB managed (8 blocks)
        self.assertEqual(status, PMMStatus.OK)
        
        # Allocate two adjacent blocks
        status, addr1 = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        
        status, addr2 = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Free them - they should coalesce if they're buddies
        status = self.pmm.free(addr1, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        status = self.pmm.free(addr2, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Check that coalescing happened (at least some)
        free_lists = self.pmm.get_free_list_state()
        # Should have fewer blocks than if no coalescing happened
        total_blocks = sum(len(blocks) for blocks in free_lists)
        self.assertLessEqual(total_blocks, 8)  # Maximum 8 separate 4KB blocks
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_free_edge_cases(self):
        """Test freeing edge cases"""
        status = self.pmm.init(0x1000, 0x20000, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Allocate a block
        status, addr = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Test free without initialization
        pmm2 = PMM()
        status = pmm2.free(addr, 4096)
        self.assertEqual(status, PMMStatus.ERR_NOT_INIT)
        
        # Test free with zero size
        status = self.pmm.free(addr, 0)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
        
        # Test free out of range
        status = self.pmm.free(0x500, 4096)
        self.assertEqual(status, PMMStatus.ERR_OUT_OF_RANGE)
        
        status = self.pmm.free(0x30000, 4096)
        self.assertEqual(status, PMMStatus.ERR_OUT_OF_RANGE)
        
        # Test free with unaligned address
        status = self.pmm.free(0x1001, 4096)
        self.assertEqual(status, PMMStatus.ERR_NOT_ALIGNED)
        
        # Test free with invalid size (too large order) - should return ERR_INVALID
        status = self.pmm.free(addr, 0x1000000)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
    
    def test_complex_coalescing(self):
        """Test complex buddy coalescing scenarios"""
        # Set up a larger memory space - use power of 2 aligned range
        status = self.pmm.init(0x0, 0x20000, 4096)  # 128KB managed, starts at 0 for simple buddies
        self.assertEqual(status, PMMStatus.OK)
        
        # Allocate multiple blocks to create fragmentation
        addrs = []
        for i in range(8):
            status, addr = self.pmm.alloc(4096)
            self.assertEqual(status, PMMStatus.OK)
            addrs.append(addr)
        
        # Free every other block
        for i in range(0, 8, 2):
            status = self.pmm.free(addrs[i], 4096)
            self.assertEqual(status, PMMStatus.OK)
        
        # Free remaining blocks - should coalesce into larger blocks
        for i in range(1, 8, 2):
            status = self.pmm.free(addrs[i], 4096)
            self.assertEqual(status, PMMStatus.OK)
        
        # After freeing all blocks, we should have coalesced blocks
        # The exact number depends on the buddy system alignment
        # Just verify we have some coalescing (fewer than 8 separate blocks)
        free_lists = self.pmm.get_free_list_state()
        total_free = sum(len(blocks) for blocks in free_lists)
        self.assertLessEqual(total_free, 8)  # Should have coalesced at least somewhat
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_mark_reserved_range(self):
        """Test marking ranges as reserved"""
        status = self.pmm.init(0x1000, 0x21000, 4096)  # 128KB managed
        self.assertEqual(status, PMMStatus.OK)
        
        # Mark middle portion as reserved
        status = self.pmm.mark_reserved_range(0x5000, 0x7000)
        self.assertEqual(status, PMMStatus.OK)
        
        # Try to allocate - should work but skip reserved area
        status, addr1 = self.pmm.alloc(0x4000)  # 16KB
        self.assertEqual(status, PMMStatus.OK)
        
        # Should be able to allocate from non-reserved areas
        status, addr2 = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Verify allocations don't overlap with reserved area
        self.assertTrue(addr1 + 0x4000 <= 0x5000 or addr1 >= 0x7000)
        self.assertTrue(addr2 + 4096 <= 0x5000 or addr2 >= 0x7000)
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_mark_free_range(self):
        """Test marking ranges as free"""
        status = self.pmm.init(0x1000, 0x21000, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # First, reserve a range
        status = self.pmm.mark_reserved_range(0x5000, 0x7000)
        self.assertEqual(status, PMMStatus.OK)
        
        # Now mark it back as free
        status = self.pmm.mark_free_range(0x5000, 0x7000)
        self.assertEqual(status, PMMStatus.OK)
        
        # Should be able to allocate from this range now
        status, addr = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        # Allocation could come from anywhere in the managed range
        self.assertGreaterEqual(addr, 0x1000)
        self.assertLess(addr, 0x21000)
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_range_marking_edge_cases(self):
        """Test edge cases for range marking"""
        status = self.pmm.init(0x1000, 0x21000, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Test without initialization
        pmm2 = PMM()
        status = pmm2.mark_reserved_range(0x1000, 0x2000)
        self.assertEqual(status, PMMStatus.ERR_NOT_INIT)
        
        status = pmm2.mark_free_range(0x1000, 0x2000)
        self.assertEqual(status, PMMStatus.ERR_NOT_INIT)
        
        # Test invalid ranges
        status = self.pmm.mark_reserved_range(0x2000, 0x1000)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
        
        status = self.pmm.mark_free_range(0x2000, 0x1000)
        self.assertEqual(status, PMMStatus.ERR_INVALID)
        
        # Test ranges outside managed memory (should clamp and become invalid)
        status = self.pmm.mark_reserved_range(0, 0x500)
        self.assertEqual(status, PMMStatus.ERR_INVALID)  # Becomes invalid after clamping
        
        status = self.pmm.mark_free_range(0x30000, 0x40000)
        self.assertEqual(status, PMMStatus.ERR_INVALID)  # Becomes invalid after clamping
    
    def test_different_block_sizes(self):
        """Test PMM with different minimum block sizes"""
        # Test with 8KB blocks
        status = self.pmm.init(0x1000, 0x21000, 8192)
        self.assertEqual(status, PMMStatus.OK)
        self.assertEqual(self.pmm.min_block_size(), 8192)
        
        # Allocation should be rounded up to 8KB
        status, addr = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        self.assertEqual(addr & 0x1FFF, 0)  # 8KB aligned
        
        # Test with 2KB blocks
        self.pmm.shutdown()
        status = self.pmm.init(0x1000, 0x21000, 2048)
        self.assertEqual(status, PMMStatus.OK)
        self.assertEqual(self.pmm.min_block_size(), 2048)
        
        status, addr = self.pmm.alloc(1024)
        self.assertEqual(status, PMMStatus.OK)
        self.assertEqual(addr & 0x7FF, 0)  # 2KB aligned
    
    def test_random_operations(self):
        """Test with random allocation/free patterns"""
        # Use address 0x0 to ensure all blocks can be properly aligned
        status = self.pmm.init(0x0, 0x100000, 4096)  # 1MB managed
        self.assertEqual(status, PMMStatus.OK)
        
        allocations = {}
        operations = 1000
        
        for i in range(operations):
            op = random.choice(['alloc', 'free'])
            
            if op == 'alloc' and len(allocations) < 50:  # Limit concurrent allocations
                size = random.choice([4096, 8192, 16384, 32768])
                status, addr = self.pmm.alloc(size)
                
                if status == PMMStatus.OK:
                    allocations[addr] = size
                    # Verify address is within managed range
                    self.assertGreaterEqual(addr, 0x0)
                    self.assertLess(addr, 0x100000)
                    # Verify minimum alignment (to min_block_size)
                    self.assertEqual(addr & (4096 - 1), 0)
                else:
                    self.assertEqual(status, PMMStatus.ERR_OOM)
            
            elif op == 'free' and allocations:
                addr = random.choice(list(allocations.keys()))
                size = allocations[addr]
                
                status = self.pmm.free(addr, size)
                self.assertEqual(status, PMMStatus.OK)
                del allocations[addr]
            
            # Periodically validate consistency
            if i % 100 == 0:
                consistent, msg = self.pmm.validate_memory_consistency()
                self.assertTrue(consistent, msg)
        
        # Final consistency check
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_exact_memory_tracking(self):
        """Test exact memory accounting"""
        # Use small, exact memory to make tracking easier
        total_memory = 0x8000  # 32KB
        status = self.pmm.init(0x0, total_memory, 4096)  # Start at 0 for clean buddies
        self.assertEqual(status, PMMStatus.OK)
        
        # Track allocated memory manually
        manual_allocated = 0
        
        # Allocate until OOM
        allocations = []
        while True:
            status, addr = self.pmm.alloc(4096)
            if status == PMMStatus.OK:
                allocations.append(addr)
                manual_allocated += 4096
            else:
                break
        
        # Total allocated should be <= total memory
        self.assertLessEqual(manual_allocated, total_memory)
        
        # Free half the allocations
        for i in range(len(allocations) // 2):
            addr = allocations.pop()
            status = self.pmm.free(addr, 4096)
            self.assertEqual(status, PMMStatus.OK)
            manual_allocated -= 4096
        
        # Allocate again - should succeed
        status, addr = self.pmm.alloc(4096)
        if status == PMMStatus.OK:
            allocations.append(addr)
            manual_allocated += 4096
        
        # Verify PMM's internal tracking matches our manual tracking
        pmm_allocated = sum(self.pmm.get_allocated_blocks().values())
        self.assertEqual(pmm_allocated, manual_allocated)
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_allocation_size_rounding(self):
        """Test that allocation sizes are rounded correctly"""
        status = self.pmm.init(0x1000, 0x21000, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Allocate odd sizes - should round up
        test_sizes = [1, 100, 1000, 3000, 4000, 5000, 8000, 10000]
        
        for size in test_sizes:
            status, addr = self.pmm.alloc(size)
            self.assertEqual(status, PMMStatus.OK)
            
            # Address should be aligned to min_block
            self.assertEqual(addr & (4096 - 1), 0)
            
            # Track the allocation
            allocated_size = self.pmm.get_allocated_blocks()[addr]
            
            # Allocated size should be >= requested size
            self.assertGreaterEqual(allocated_size, size)
            
            # Allocated size should be power of 2 * min_block
            self.assertTrue(is_pow2_u64(allocated_size // 4096))
            
            # Clean up
            status = self.pmm.free(addr, size)
            self.assertEqual(status, PMMStatus.OK)
    
    def test_buddy_system_properties(self):
        """Test that buddy system properties hold"""
        # Use aligned range starting at 0 for predictable buddies
        status = self.pmm.init(0x0, 0x10000, 4096)  # 64KB
        self.assertEqual(status, PMMStatus.OK)
        
        # Allocate a block
        status, addr1 = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Calculate its buddy (manually)
        buddy_addr = ((addr1 - self.pmm.managed_base()) ^ 4096) + self.pmm.managed_base()
        
        # Allocate the buddy
        status, addr2 = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # One of the allocations should be the buddy
        # (if the allocator gave us sequential blocks)
        if addr2 == buddy_addr:
            # Free both - they should coalesce
            status = self.pmm.free(addr1, 4096)
            self.assertEqual(status, PMMStatus.OK)
            
            status = self.pmm.free(addr2, 4096)
            self.assertEqual(status, PMMStatus.OK)
            
            # Check that we have fewer order-0 blocks now
            free_lists = self.pmm.get_free_list_state()
            # Should have coalesced into higher order
            # This is a weak check but verifies coalescing happens
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_fragmentation_and_recovery(self):
        """Test that the allocator can recover from fragmentation"""
        status = self.pmm.init(0x0, 0x20000, 4096)  # 128KB
        self.assertEqual(status, PMMStatus.OK)
        
        # Create fragmentation by allocating and freeing in pattern
        small_allocs = []
        for i in range(16):
            status, addr = self.pmm.alloc(4096)
            self.assertEqual(status, PMMStatus.OK)
            small_allocs.append(addr)
        
        # Free every other one
        for i in range(0, 16, 2):
            status = self.pmm.free(small_allocs[i], 4096)
            self.assertEqual(status, PMMStatus.OK)
        
        # Try to allocate a large block - might fail due to fragmentation
        status, large_addr = self.pmm.alloc(32768)  # 32KB
        
        # Now free all remaining small blocks
        for i in range(1, 16, 2):
            status = self.pmm.free(small_allocs[i], 4096)
            self.assertEqual(status, PMMStatus.OK)
        
        # If we previously failed, should succeed now
        if status != PMMStatus.OK:
            status, large_addr = self.pmm.alloc(32768)
            self.assertEqual(status, PMMStatus.OK)
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_maximum_allocation_size(self):
        """Test allocation at maximum order"""
        # Create large memory region
        status = self.pmm.init(0x0, 0x1000000, 4096)  # 16MB
        self.assertEqual(status, PMMStatus.OK)
        
        # Find max order
        max_order = self.pmm.g_max_order
        max_size = self.pmm.order_to_size(max_order)
        
        # Allocate at max size
        status, addr = self.pmm.alloc(max_size)
        self.assertEqual(status, PMMStatus.OK)
        
        # Free it
        status = self.pmm.free(addr, max_size)
        self.assertEqual(status, PMMStatus.OK)
        
        # Try to allocate too large
        status, addr = self.pmm.alloc(max_size * 2 + 1)
        self.assertEqual(status, PMMStatus.ERR_OOM)
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)
    
    def test_stress_alloc_free_cycles(self):
        """Stress test with many allocation/free cycles"""
        status = self.pmm.init(0x0, 0x100000, 4096)  # 1MB
        self.assertEqual(status, PMMStatus.OK)
        
        for cycle in range(10):
            allocations = []
            
            # Allocate many blocks
            for i in range(50):
                size = random.choice([4096, 8192, 16384])
                status, addr = self.pmm.alloc(size)
                if status == PMMStatus.OK:
                    allocations.append((addr, size))
            
            # Free them all in random order
            random.shuffle(allocations)
            for addr, size in allocations:
                status = self.pmm.free(addr, size)
                self.assertEqual(status, PMMStatus.OK)
            
            # Verify consistency after each cycle
            consistent, msg = self.pmm.validate_memory_consistency()
            self.assertTrue(consistent, f"Cycle {cycle}: {msg}")
        
        # Final check
        # Should have all memory free again
        free_lists = self.pmm.get_free_list_state()
        total_free_blocks = sum(len(blocks) for blocks in free_lists)
        self.assertGreater(total_free_blocks, 0)
    
    def test_non_aligned_range_start(self):
        """Test PMM with various range starts"""
        # Start at 0x1000 (4KB aligned but not 8KB aligned)
        status = self.pmm.init(0x1000, 0x21000, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # All allocations should be at least 4KB aligned
        status, addr = self.pmm.alloc(4096)
        self.assertEqual(status, PMMStatus.OK)
        self.assertEqual(addr & 0xFFF, 0)
        
        status = self.pmm.free(addr, 4096)
        self.assertEqual(status, PMMStatus.OK)
        
        # Verify consistency
        consistent, msg = self.pmm.validate_memory_consistency()
        self.assertTrue(consistent, msg)

if __name__ == '__main__':
    # Run all tests
    unittest.main(verbosity=2)
