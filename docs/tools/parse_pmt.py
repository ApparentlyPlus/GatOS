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

def main():
    parser = PageTableParser()
    
    try:
        parser.parse_file('dump.txt')
        print("Successfully parsed page table hierarchy from dump.txt")
    except FileNotFoundError:
        print("Error: dump.txt not found in the current directory")
        return

    while True:
        print("\nGive me a virtual address (or press Enter to quit): ", end='')
        try:
            addr_input = input().strip()
            if not addr_input:
                break

            result = parser.virtual_to_physical(addr_input)
            print(f"\nVirtual Address: 0x{int(addr_input, 16):X}")
            
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


main()