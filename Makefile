# Toolchain configuration
CC := x86_64-elf-gcc
LD := x86_64-elf-ld

# Compilation and preprocessing flags
CFLAGS := -m64 -ffreestanding -nostdlib -fno-pic -mcmodel=kernel -I src/headers -g
CPPFLAGS := -I src/headers -D__ASSEMBLER__
LDFLAGS := -n -nostdlib -T targets/x86_64/linker.ld --no-relax -g

# Directories
SRC_DIR := src/impl
HEADER_DIR := src/headers
BUILD_DIR := build
DIST_DIR := dist/x86_64
ISO_DIR := targets/x86_64/iso

# Discover all C and Assembly sources recursively
C_SRC_FILES := $(shell find $(SRC_DIR) -type f -name '*.c')
ASM_SRC_FILES := $(shell find $(SRC_DIR) -type f -name '*.S')

# Generate corresponding object file paths
C_OBJ_FILES := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRC_FILES))
ASM_OBJ_FILES := $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(ASM_SRC_FILES))
OBJ_FILES := $(C_OBJ_FILES) $(ASM_OBJ_FILES)

# Default target
.PHONY: all
all: iso

# Compile C source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

# Compile Assembly (.S) source files with preprocessing
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(@D)
	$(CC) -c $(CPPFLAGS) $< -o $@

# Link everything into a flat binary
.PHONY: build
build: $(OBJ_FILES)
	@mkdir -p $(DIST_DIR)
	$(LD) $(LDFLAGS) -o $(DIST_DIR)/kernel.bin $^

# Generate ISO image
.PHONY: iso
iso: build
	@mkdir -p $(ISO_DIR)/boot
	cp $(DIST_DIR)/kernel.bin $(ISO_DIR)/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o $(DIST_DIR)/kernel.iso $(ISO_DIR)

# Clean all build and dist files
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR) dist $(ISO_DIR)/boot/kernel.bin
