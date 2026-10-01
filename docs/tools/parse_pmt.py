import re
from collections import defaultdict

ENTRY = re.compile(r'^(PML4|PDPT|PD|PT)\[([0-9A-F]+)\]:\s+([0-9A-F]+)\s+->\s+(PHYS|PDPT|PD|PT)')

def parse_addr(s):
    if s.startswith('0x') or s.startswith('0X'):
        return int(s[2:], 16)
    if s.startswith('ffffffff'):
        return int(s[8:], 16) | (0xffffffff << 32)
    return int(s, 16)

# 48-bit canonical -> 64-bit: sign-extend bit 47
def canonical(pml4, pdpt, pd, pt):
    a = (pml4 << 39) | (pdpt << 30) | (pd << 21) | (pt << 12)
    return a | (0xFFFF << 48) if a & (1 << 47) else a

class PageTables:
    def __init__(self):
        # pml4 -> pdpt -> pd -> pt -> entry
        self.tree = defaultdict(lambda: defaultdict(lambda: defaultdict(dict)))
        self.cur = [None, None, None]

    def parse_line(self, line):
        m = ENTRY.match(line.strip())
        if not m:
            return
        kind, idx, val, _ = m.groups()
        idx, val = int(idx, 16), int(val, 16)

        if kind == 'PML4':
            self.cur = [idx, None, None]
        elif kind == 'PDPT':
            self.cur[1:] = [idx, None]
        elif kind == 'PD':
            self.cur[2] = idx
        else:
            a, b, c = self.cur
            self.tree[a][b][c][idx] = val

    def load(self, path):
        with open(path) as f:
            for line in f:
                self.parse_line(line)

    def translate(self, addr):
        if isinstance(addr, str):
            addr = parse_addr(addr)

        idx = [(addr >> 39) & 0x1FF, (addr >> 30) & 0x1FF, (addr >> 21) & 0x1FF, (addr >> 12) & 0x1FF]
        node, path = self.tree, []
        for name, i in zip(("PML4", "PDPT", "PD", "PT"), idx):
            if i not in node:
                return {'error': f"{name}[{i:04X}] -> Not existent", 'valid_path': path}
            path = path + [f"{name}[{i:04X}]"]
            node = node[i]

        return {
            'pml4_index': idx[0],
            'pdpt_index': idx[1],
            'pd_index': idx[2],
            'pt_index': idx[3],
            'phys_addr': (node & ~0xFFF) | (addr & 0xFFF),
            'phys_page': node,
            'valid': True
        }

    # Contiguous [virt_start, virt_end, phys_start, phys_end] runs, pages merged across tables.
    def ranges(self):
        pages = sorted((canonical(a, b, c, d), e & ~0xFFF)
                       for a, l3 in self.tree.items() for b, l2 in l3.items()
                       for c, l1 in l2.items() for d, e in l1.items())
        runs = []
        for v, p in pages:
            if runs and v == runs[-1][1] + 1 and p == runs[-1][3] + 1:
                runs[-1][1] += 0x1000
                runs[-1][3] += 0x1000
            else:
                runs.append([v, v + 0xFFF, p, p + 0xFFF])
        return runs

    def show_ranges(self):
        runs = self.ranges()
        if not runs:
            print("No mapped ranges found.")
            return

        print("\nMapped Virtual to Physical Ranges:\n")
        for vs, ve, ps, pe in runs:
            print(f"[0x{vs:016X}, 0x{ve:016X}] -> [0x{ps:016X}, 0x{pe:016X}]")
        print(f"\nTotal: {len(runs)} contiguous mapping range(s)")

def main():
    pt = PageTables()

    try:
        pt.load('dump.txt')
        print("Successfully parsed page table hierarchy from dump.txt")
    except FileNotFoundError:
        print("Error: dump.txt not found in the current directory")
        return

    pt.show_ranges()

    while True:
        print("\nGive me a virtual address (or press Enter to quit): ", end='')
        try:
            s = input().strip()
            if not s:
                break

            r = pt.translate(s)
            print(f"\nVirtual Address: 0x{parse_addr(s):016X}")

            if 'valid' in r:
                print(f"  PML4 Index : 0x{r['pml4_index']:03X}")
                print(f"  PDPT Index : 0x{r['pdpt_index']:03X}")
                print(f"  PD   Index : 0x{r['pd_index']:03X}")
                print(f"  PT   Index : 0x{r['pt_index']:03X}")
                print(f"Phys addr: 0x{r['phys_addr']:X}")
            else:
                print("Mapping path:")
                for step in r['valid_path']:
                    print(f"  {step}")
                print(f"  {r['error']}")
                print("\nNo physical mapping found for this virtual address")

        except ValueError:
            print("Invalid input. Please enter a valid hexadecimal address.")
        except (EOFError, KeyboardInterrupt):
            print("\nExiting...")
            break

if __name__ == "__main__":
    main()
