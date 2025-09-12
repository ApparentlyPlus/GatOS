def extract_page_table_indices(virtual_address):
    if not (0 <= virtual_address <= 0xFFFFFFFFFFFFFFFF):
        raise ValueError("Address must be a 64-bit integer")

    pt_index    = (virtual_address >> 12) & 0x1FF
    pd_index    = (virtual_address >> 21) & 0x1FF
    pdpt_index  = (virtual_address >> 30) & 0x1FF
    pml4_index  = (virtual_address >> 39) & 0x1FF

    print(f"Virtual Address: 0x{virtual_address:016X}")
    print(f"  PML4 Index : 0x{pml4_index:04X}")
    print(f"  PDPT Index : 0x{pdpt_index:04X}")
    print(f"  PD   Index : 0x{pd_index:04X}")
    print(f"  PT   Index : 0x{pt_index:04X}")

while True:
    addr = int(input("> Enter address: "), 16)
    extract_page_table_indices(addr)
    print()


