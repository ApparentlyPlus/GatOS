# Documentation Tools

This section provides a collection of useful tools referenced throughout the documentation that can help you understand, bootstrap, or debug your code.

## Available Tools

### File: [`dump_pmt.S`](dump_pmt.S)

**Description:** 

A 32-bit GAS assembly file for outputting page table mappings to the QEMU serial output. This tool is designed for early-stage troubleshooting in 32-bit mode.

**Usage Instructions:**

1. **Add to Project:** Place `dump_pmt.S` along with your other assembly files in your project directory.

2. **Configure Includes:** Replace the placeholder `#include` directive for `paging.h` with the actual file that contains your `KERNEL_P2V/V2P` macros.

3. **Declare Global Variables:** In your main assembly file, declare your PML4 address as a global variable.

4. **Set Up External Reference:** Add the following declaration to your main assembly (`.S`) kernel file:
   ```assembly
   .extern dbg_dump_pmt_asm
   ```

5. **Call the Function:** After setting up your page tables, call the dump function:
   ```assembly
   call dbg_dump_pmt_asm
   ```

This will output the page table mappings in your QEMU serial output. To use it, you can run:

```
qemu-system-x86_64 -cdrom kernel.iso -serial stdio > dump.txt
```

Then, you can throw `dump.txt` in [`parse_pmt.py`](parse_pmt.py) in order to play around with your page tables, see what's mapped and what isn't, and debug unmapped addresses.

**Note:** For 64-bit mode troubleshooting, since you can call C code directly, you should include [`dump_pmt.c`](dump_pmt.c).

---

### File: [`dump_pmt.c`](dump_pmt.c)

**Description:** 

A C-based debugging tool for dumping complete page table hierarchy information to the QEMU serial console. This tool is designed for 64-bit mode troubleshooting.

**Prerequisites:** 

Your `paging.h` header must define the following macros and constants:

```c
#define KERNEL_VIRTUAL_BASE     0xFFFFFFFF80000000
#define PRESENT                 (1ULL << 0)
#define PAGE_SIZE               0x1000UL
#define PAGE_ENTRIES            512
#define PAGE_MASK               0xFFFFF000

#ifdef __ASSEMBLER__
    #define KERNEL_V2P(a) ((a) - KERNEL_VIRTUAL_BASE)
    #define KERNEL_P2V(a) ((a) + KERNEL_VIRTUAL_BASE)
#else
    #include <stdint.h>
    #define KERNEL_V2P(a) ((uintptr_t)(a) & ~KERNEL_VIRTUAL_BASE)
    #define KERNEL_P2V(a) ((uintptr_t)(a) | KERNEL_VIRTUAL_BASE)
#endif
```

**Usage Instructions:**

1. **Include the File:** Add `dump_pmt.c` to your project source files.

2. **Update Include Path:** Modify the `#include <path/to/paging.h>` line to point to your actual `paging.h` header file location.

3. **Create a Header:** Create a header file `dump_pmt.h` and expose whatever functions you need.

4. **Dump Page Tables:** Call `dbg_dump_pmt()` after your page tables are fully set up to output the complete page table hierarchy to the serial console.

**Example Integration:**

In C, you would do something like:

```c
// In your kernel main function
#include <dump_pmt.h>

void kernel_main(void) {

    [...] 

    // Set up your page tables
    setup_paging();
    
    // Dump page table structure to serial output
    dbg_dump_pmt();
    
    [...]
}
```

or in assemby, something like:

```assembly
; Declare the external function
.extern dbg_dump_pmt

.section .text
.code 64

[...]

call dbg_dump_pmt
```

This will output the page table mappings in your QEMU serial output. To use it, you can run:

```
qemu-system-x86_64 -cdrom kernel.iso -serial stdio > dump.txt
```

Then, you can throw `dump.txt` in [`parse_pmt.py`](parse_pmt.py) in order to play around with your page tables, see what's mapped and what isn't, and debug unmapped addresses.

---

### File: [`parse_pmt.py`](parse_pmt.py)

**Description:** 

A Python-based page table analyzer that parses the output from `dump_pmt.c` or `parse_pmt.S` and provides interactive virtual-to-physical address translation. This tool helps debug and understand your memory mapping structure by displaying all mapped memory ranges and allowing real-time address translation queries.

**Features:**
- Parses page table hierarchy from serial dump output
- Displays all contiguous virtual-to-physical memory mappings
- Provides interactive virtual address translation based on your mappings
- Shows detailed page table walk information for both valid and invalid mappings

**Usage Instructions:**

1. **Capture Output:** First, run your kernel with `dump_pmt.c` or `parse_pmt.S` integrated and capture the serial output to a file named `dump.txt`. Place that file in the same folder as the script.

2. **Run the Parser:** Execute the Python script:
   ```bash
   python3 parse_pmt.py
   ```

3. **Interactive Translation:** The script will automatically display all mapped memory ranges and then prompt for virtual addresses to translate.

**Example Usage:**

```bash
$ python3 parse_pmt.py
Successfully parsed page table hierarchy from dump.txt

Mapped Virtual to Physical Ranges:

[0xFFFF800000000000, 0xFFFF800007FDFFFF] -> [0x0000000000000000, 0x0000000007FDFFFF]
[0xFFFFFFFF80000000, 0xFFFFFFFF8026BFFF] -> [0x0000000000000000, 0x000000000026BFFF]

Total: 2 contiguous mapping range(s)

Give me a virtual address (or press Enter to quit): 0xFFFFFFFF80001000

Virtual Address: 0xFFFFFFFF80001000
  PML4 Index : 0x1FF
  PDPT Index : 0x1FE
  PD   Index : 0x000
  PT   Index : 0x001
Phys addr: 0x1000

Give me a virtual address (or press Enter to quit): 0xFFFF812000000000    

Virtual Address: 0xFFFF812000000000
Mapping path:
  PML4[0102] -> Not existent

No physical mapping found for this virtual address

Give me a virtual address (or press Enter to quit): 0xFFFF800012000000    

Virtual Address: 0xFFFF800012000000
Mapping path:
  PML4[0100]
  PDPT[0000]
  PD[0090] -> Not existent

No physical mapping found for this virtual address
```

**Input Formats Accepted:**
- `0x...` (hexadecimal with 0x prefix)
- `ffffffff...` (hexadecimal without 0x prefix)
- Decimal addresses (though hexadecimal is recommended)

**Output Information:**
- **Mapped Ranges:** Shows all contiguous virtual-to-physical memory regions
- **Valid Mappings:** Displays complete page table walk (PML4→PDPT→PD→PT indices) and physical address
- **Invalid Mappings:** Shows where the page table walk fails and which level is missing

This tool is particularly useful for verifying that your kernel's memory mappings are set up correctly and for debugging page fault issues by identifying exactly where in the page table hierarchy a mapping is missing.

---

### File: [`retrieve_src.py`](retrieve_src.py)

**Description:** 

A Python utility script that scans the current directory and its subdirectories for source code files, displays their contents organized by file extension, and automatically copies everything to the clipboard. This tool is particularly useful for sharing code context with LLMs, collaborators, or for documentation purposes.

**Features:**
- Recursively scans all files in the current directory and subdirectories
- Filters files by specific extensions or shows all files
- Displays file contents with proper headers
- Automatically copies all output to clipboard for easy sharing
- Handles UTF-8 encoding and error cases gracefully

**Usage Instructions:**

1. **Run the Script:** Execute the script from the directory you want to scan:
   ```bash
   python3 retrieve_src.py
   ```

2. **Select File Extensions:** The script will show available extensions and prompt for your selection:
   - Enter specific extensions (comma-separated): `py, c, h, md`
   - Enter `all` to include every file type
   - Press Enter after providing your selection

3. **Automatic Clipboard Copy:** All displayed content is automatically copied to your clipboard for easy pasting.

**Example Usage:**
```bash
$ python3 retrieve_src.py
Scanning for files in: /home/user/my_project

Available file extensions found:
c, h, md, py, s, txt

Enter desired file extensions (e.g., py, json, txt) or 'all': c,h,py

--- File Contents ---

kernel.c:
#include <stdint.h>
#include "paging.h"

void kernel_main(void) {
    // Kernel initialization code...
}

paging.h:
#ifndef PAGING_H
#define PAGING_H

#define PAGE_SIZE 4096
// ... header contents

retrieve_src.py:
import os
import pyperclip
# ... script contents

All output copied to clipboard!
```

**Input Options:**
- **Specific extensions:** `c, h, py, md` (comma-separated, case-insensitive)
- **All files:** `all` (includes every file type except the script itself)
- **Mixed extensions:** `py, json, txt` (any combination supported)

**Output Features:**
- Each file is displayed with a clear header: `filename:`
- Contents are printed exactly as they appear in the files
- Error messages for files that can't be read or decoded
- All output is concatenated and copied to clipboard automatically

**Dependencies:**
- Python 3.x
- `pyperclip` library (install with `pip install pyperclip`)

---

### File: [`virt_breakdown.py`](virt_breakdown.py)

**Description:** 

An interactive Python utility that breaks down 64-bit virtual addresses into their constituent page table indices. This tool helps understand the x86-64 paging structure by showing exactly which PML4, PDPT, PD, and PT entries need to be populated for a given virtual address to be properly mapped.

**Features:**

- Interactive prompt for virtual address input
- Displays all four levels of page table indices
- Supports both canonical and non-canonical 64-bit addresses
- Clear, formatted output showing the hierarchical breakdown

**Usage Instructions:**

1. **Run the Script:** Execute the script:
   ```bash
   python3 virt_breakdown.py
   ```

2. **Enter Virtual Addresses:** Input virtual addresses in hexadecimal format (with or without `0x` prefix). The script will continuously prompt for addresses until terminated.

3. **Exit:** Use `Ctrl+C` to exit the interactive loop.

**Example Usage:**
```bash
$ python3 virt_breakdown.py

> Enter address: 0xFFFFFFFF80000000
Virtual Address: 0xFFFFFFFF80000000
  PML4 Index : 0x01FF
  PDPT Index : 0x01FE
  PD   Index : 0x0000
  PT   Index : 0x0000

> Enter address: 0xFFFF800000000000
Virtual Address: 0xFFFF800000000000
  PML4 Index : 0x0100
  PDPT Index : 0x0000
  PD   Index : 0x0000
  PT   Index : 0x0000

> Enter address: 0x400000
Virtual Address: 0x0000000000400000
  PML4 Index : 0x0000
  PDPT Index : 0x0000
  PD   Index : 0x0002
  PT   Index : 0x0000
```

**Input Formats:**
- `0xFFFFFFFF80000000` (hexadecimal with `0x` prefix)
- `ffffffff80000000` (hexadecimal without prefix)
- `400000` (hexadecimal without prefix)
- Any valid 64-bit hexadecimal value

**Output Explanation:**
The breakdown shows the four levels of x86-64 page table hierarchy:
- **PML4 Index** (bits 39-47): Points to the Page Map Level 4 entry
- **PDPT Index** (bits 30-39): Points to the Page Directory Pointer Table entry  
- **PD Index** (bits 21-30): Points to the Page Directory entry
- **PT Index** (bits 12-21): Points to the Page Table entry

**Technical Details:**
- Uses standard x86-64 4-level paging structure
- Handles both canonical addresses (with proper sign extension) and non-canonical addresses
- Validates that addresses are within 64-bit range

This tool is particularly valuable when working with memory management code, as it provides immediate insight into which page table entries need to be set up for any given virtual address mapping.