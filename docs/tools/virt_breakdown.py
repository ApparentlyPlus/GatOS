def show_indices(addr):
    if not (0 <= addr <= 0xFFFFFFFFFFFFFFFF):
        raise ValueError("Address must be a 64-bit integer")

    pt = (addr >> 12) & 0x1FF
    pd = (addr >> 21) & 0x1FF
    pdpt = (addr >> 30) & 0x1FF
    pml4 = (addr >> 39) & 0x1FF

    print(f"Virtual Address: 0x{addr:016X}")
    print(f"  PML4 Index : 0x{pml4:04X} ({pml4})")
    print(f"  PDPT Index : 0x{pdpt:04X} ({pdpt})")
    print(f"  PD   Index : 0x{pd:04X} ({pd})")
    print(f"  PT   Index : 0x{pt:04X} ({pt})")

while True:
    show_indices(int(input("> Enter address: "), 16))
    print()
