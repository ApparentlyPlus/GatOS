import re
from collections import defaultdict

class PageTableParser:
    def __init__(self):
        self.pml4 = defaultdict(lambda: defaultdict(lambda: defaultdict(dict)))
        self.current_pml4 = None
        self.current_pdpt = None
        self.current_pd = None

    def parse_line(self, line):
        line = line.strip()
        if not line:
            return

        # Match lines like "PML4[0000]: 00017003 -> PDPT"
        match = re.match(r'^(PML4|PDPT|PD|PT)\[([0-9A-F]+)\]:\s+([0-9A-F]+)\s+->\s+(PHYS|PDPT|PD|PT)', line)
        if not match:
            return

        entry_type, index_hex, value_hex, target = match.groups()
        index = int(index_hex, 16)
        value = int(value_hex, 16)

        if entry_type == 'PML4':
            self.current_pml4 = index
            self.current_pdpt = None
            self.current_pd = None
        elif entry_type == 'PDPT':
            self.current_pdpt = index
            self.current_pd = None
        elif entry_type == 'PD':
            self.current_pd = index

        if entry_type == 'PT':
            # Add PT entry
            self.pml4[self.current_pml4][self.current_pdpt][self.current_pd][index] = value

    def parse_file(self, filename):
        with open(filename, 'r') as file:
            for line in file:
                self.parse_line(line)

    def virtual_to_physical(self, virtual_addr):
        if isinstance(virtual_addr, str):
            if virtual_addr.startswith('0x') or virtual_addr.startswith('0X'):
                virtual_addr = int(virtual_addr[2:], 16)
            elif virtual_addr.startswith('ffffffff'):
                virtual_addr = int(virtual_addr[8:], 16) | (0xffffffff << 32)
            else:
                virtual_addr = int(virtual_addr, 16)

        # Break down the virtual address into indices
        pml4_index = (virtual_addr >> 39) & 0x1FF
        pdpt_index = (virtual_addr >> 30) & 0x1FF
        pd_index = (virtual_addr >> 21) & 0x1FF
        pt_index = (virtual_addr >> 12) & 0x1FF
        offset = virtual_addr & 0xFFF

        # Check each level and report where it fails
        if pml4_index not in self.pml4:
            return {
                'error': f"PML4[{pml4_index:04X}] -> Not existent",
                'valid_path': []
            }

        if pdpt_index not in self.pml4[pml4_index]:
            return {
                'error': f"PDPT[{pdpt_index:04X}] -> Not existent",
                'valid_path': [f"PML4[{pml4_index:04X}]"]
            }

        if pd_index not in self.pml4[pml4_index][pdpt_index]:
            return {
                'error': f"PD[{pd_index:04X}] -> Not existent",
                'valid_path': [
                    f"PML4[{pml4_index:04X}]",
                    f"PDPT[{pdpt_index:04X}]"
                ]
            }

        if pt_index not in self.pml4[pml4_index][pdpt_index][pd_index]:
            return {
                'error': f"PT[{pt_index:04X}] -> Not existent",
                'valid_path': [
                    f"PML4[{pml4_index:04X}]",
                    f"PDPT[{pdpt_index:04X}]",
                    f"PD[{pd_index:04X}]"
                ]
            }

        phys_page = self.pml4[pml4_index][pdpt_index][pd_index][pt_index]
        phys_addr = (phys_page & ~0xFFF) | offset
        return {
            'pml4_index': pml4_index,
            'pdpt_index': pdpt_index,
            'pd_index': pd_index,
            'pt_index': pt_index,
            'phys_addr': phys_addr,
            'phys_page': phys_page,
            'valid': True
        }

    def _build_virtual_addr(self, pml4_idx, pdpt_idx, pd_idx, pt_idx):
        """Build a canonical virtual address from page table indices"""
        # Build the 48-bit virtual address
        addr_48 = (pml4_idx << 39) | (pdpt_idx << 30) | (pd_idx << 21) | (pt_idx << 12)
        
        # Check if this is a high-half address (bit 47 set)
        if addr_48 & (1 << 47):
            # Sign extend to 64 bits (upper 16 bits set to 1)
            addr_64 = addr_48 | (0xFFFF << 48)
        else:
            # Lower half address (upper 16 bits set to 0)
            addr_64 = addr_48
        
        return addr_64

    def get_mapped_ranges(self):
        """Get all mapped virtual to physical ranges, merging contiguous ranges"""
        ranges = []
        
        # Iterate through all page table entries
        for pml4_idx, pdpt_dict in self.pml4.items():
            for pdpt_idx, pd_dict in pdpt_dict.items():
                for pd_idx, pt_dict in pd_dict.items():
                    # Sort PT entries by index
                    pt_indices = sorted(pt_dict.keys())
                    
                    if not pt_indices:
                        continue
                    
                    # Start with first PT entry
                    current_virt_start = self._build_virtual_addr(pml4_idx, pdpt_idx, pd_idx, pt_indices[0])
                    current_phys_start = pt_dict[pt_indices[0]] & ~0xFFF
                    current_virt_end = current_virt_start + 0xFFF
                    current_phys_end = current_phys_start + 0xFFF
                    
                    for i in range(1, len(pt_indices)):
                        # Calculate current virtual and physical addresses
                        virt_addr = self._build_virtual_addr(pml4_idx, pdpt_idx, pd_idx, pt_indices[i])
                        phys_addr = pt_dict[pt_indices[i]] & ~0xFFF
                        
                        # Check if this entry is contiguous with previous
                        if (virt_addr == current_virt_end + 1 and 
                            phys_addr == current_phys_end + 1):
                            # Extend current range
                            current_virt_end = virt_addr + 0xFFF
                            current_phys_end = phys_addr + 0xFFF
                        else:
                            # Save current range and start new one
                            ranges.append({
                                'virt_start': current_virt_start,
                                'virt_end': current_virt_end,
                                'phys_start': current_phys_start,
                                'phys_end': current_phys_end
                            })
                            
                            current_virt_start = virt_addr
                            current_virt_end = virt_addr + 0xFFF
                            current_phys_start = phys_addr
                            current_phys_end = phys_addr + 0xFFF
                    
                    # Add the last range
                    ranges.append({
                        'virt_start': current_virt_start,
                        'virt_end': current_virt_end,
                        'phys_start': current_phys_start,
                        'phys_end': current_phys_end
                    })
        
        # Merge ranges that might span across different page directories/tables
        if not ranges:
            return ranges
            
        # Sort by virtual start address
        ranges.sort(key=lambda x: x['virt_start'])
        
        merged_ranges = []
        current_range = ranges[0]
        
        for i in range(1, len(ranges)):
            next_range = ranges[i]
            
            # Check if ranges can be merged (virtually and physically contiguous)
            if (next_range['virt_start'] == current_range['virt_end'] + 1 and
                next_range['phys_start'] == current_range['phys_end'] + 1):
                # Merge ranges
                current_range['virt_end'] = next_range['virt_end']
                current_range['phys_end'] = next_range['phys_end']
            else:
                # Add current range and move to next
                merged_ranges.append(current_range)
                current_range = next_range
        
        merged_ranges.append(current_range)
        return merged_ranges

    def display_mapped_ranges(self):
        """Display all mapped ranges in the requested format"""
        ranges = self.get_mapped_ranges()
        
        if not ranges:
            print("No mapped ranges found.")
            return
        
        print("\nMapped Virtual to Physical Ranges:\n")
        
        for i, range_info in enumerate(ranges, 1):
            virt_start = range_info['virt_start']
            virt_end = range_info['virt_end']
            phys_start = range_info['phys_start']
            phys_end = range_info['phys_end']
            
            print(f"[0x{virt_start:016X}, 0x{virt_end:016X}] -> "
                  f"[0x{phys_start:016X}, 0x{phys_end:016X}]")
        
        print(f"\nTotal: {len(ranges)} contiguous mapping range(s)")

def main():
    parser = PageTableParser()
    
    try:
        parser.parse_file('dump.txt')
        print("Successfully parsed page table hierarchy from dump.txt")
    except FileNotFoundError:
        print("Error: dump.txt not found in the current directory")
        return

    # Display mapped ranges before asking for user input
    parser.display_mapped_ranges()

    while True:
        print("\nGive me a virtual address (or press Enter to quit): ", end='')
        try:
            addr_input = input().strip()
            if not addr_input:
                break

            result = parser.virtual_to_physical(addr_input)
            
            # Handle canonical address formatting for display
            if addr_input.startswith('0x') or addr_input.startswith('0X'):
                virt_addr_int = int(addr_input[2:], 16)
            elif addr_input.startswith('ffffffff'):
                virt_addr_int = int(addr_input[8:], 16) | (0xffffffff << 32)
            else:
                virt_addr_int = int(addr_input, 16)
            
            print(f"\nVirtual Address: 0x{virt_addr_int:016X}")
            
            if 'valid' in result:
                print(f"  PML4 Index : 0x{result['pml4_index']:03X}")
                print(f"  PDPT Index : 0x{result['pdpt_index']:03X}")
                print(f"  PD   Index : 0x{result['pd_index']:03X}")
                print(f"  PT   Index : 0x{result['pt_index']:03X}")
                print(f"Phys addr: 0x{result['phys_addr']:X}")
            else:
                print("Mapping path:")
                for step in result['valid_path']:
                    print(f"  {step}")
                print(f"  {result['error']}")
                print("\nNo physical mapping found for this virtual address")

        except (ValueError, EOFError):
            print("Invalid input. Please enter a valid hexadecimal address.")
            continue
        except KeyboardInterrupt:
            print("\nExiting...")
            break


if __name__ == "__main__":
    main()