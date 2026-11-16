import re
from collections import defaultdict

ENTRY = re.compile(r'^(PML4|PDPT|PD|PT)\[([0-9A-F]+)\]:\s+([0-9A-F]+)\s+->\s+(PHYS|PDPT|PD|PT)')

def parse_addr(s):
    if s.startswith('0x') or s.startswith('0X'):
        return int(s[2:], 16)
    if s.startswith('ffffffff'):
        return int(s[8:], 16) | (0xffffffff << 32)
    return int(s, 16)

ADDR_4K = 0x000FFFFFFFFFF000
ADDR_2M = 0x000FFFFFFFE00000

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
        kind, idx, val, target = m.groups()
        idx, val = int(idx, 16), int(val, 16)

        if kind == 'PML4':
            self.cur = [idx, None, None]
        elif kind == 'PDPT':
            self.cur[1:] = [idx, None]
        elif kind == 'PD' and target == 'PHYS':
            # 2MiB page, stored in place of a PT
            a, b = self.cur[0], self.cur[1]
            self.tree[a][b][idx] = val & ADDR_2M
        elif kind == 'PD':
            self.cur[2] = idx
        else:
            a, b, c = self.cur
            self.tree[a][b][c][idx] = val & ADDR_4K

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
            if name == "PD" and isinstance(node, int):
                return {
                    'pml4_index': idx[0],
                    'pdpt_index': idx[1],
                    'pd_index': idx[2],
                    'pt_index': idx[3],
                    'phys_addr': node | (addr & 0x1FFFFF),
                    'phys_page': node,
                    'valid': True
                }

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
        pages = []
        for a, l3 in self.tree.items():
            for b, l2 in l3.items():
                for c, l1 in l2.items():
                    if isinstance(l1, int):
                        pages.append((canonical(a, b, c, 0), l1, 0x200000))
                    else:
                        pages.extend((canonical(a, b, c, d), e, 0x1000) for d, e in l1.items())

        runs = []
        for v, p, size in sorted(pages):
            if runs and v == runs[-1][1] + 1 and p == runs[-1][3] + 1:
                runs[-1][1] += size
                runs[-1][3] += size
            else:
                runs.append([v, v + size - 1, p, p + size - 1])
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
