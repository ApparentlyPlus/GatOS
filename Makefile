# Toolchain configuration
CC := x86_64-elf-gcc
LD := x86_64-elf-ld

# Compilation and preprocessing flags
CFLAGS_FAST := -O3 -fomit-frame-pointer -fpredictive-commoning -fstrict-aliasing
CFLAGS := -m64 -ffreestanding -nostdlib -fno-pic -mcmodel=kernel -I src/headers
CPPFLAGS := -I src/headers -D__ASSEMBLER__
LDFLAGS := -n -nostdlib -T targets/x86_64/linker.ld --no-relax -g

# Directories
SRC_DIR := src/impl
HEADER_DIR := src/headers
BUILD_DIR := build
DIST_DIR := dist/x86_64
ISO_DIR := targets/x86_64/iso

# UEFI output path (we will create this file)
UEFI_DIR := $(ISO_DIR)/EFI/BOOT
UEFI_GRUB := $(UEFI_DIR)/BOOTX64.EFI

# Discover all C and Assembly sources recursively
C_SRC_FILES := $(shell find $(SRC_DIR) -type f -name '*.c')
ASM_SRC_FILES := $(shell find $(SRC_DIR) -type f -name '*.S')

# Generate corresponding object file paths
C_OBJ_FILES := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRC_FILES))
ASM_OBJ_FILES := $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(ASM_SRC_FILES))
OBJ_FILES := $(C_OBJ_FILES) $(ASM_OBJ_FILES)

# Extract kernel version from source files (looks for KERNEL_VERSION = "vX.X.X-*";)
KERNEL_VERSION := $(shell grep -hr 'KERNEL_VERSION\s*=\s*"[^"]*"' $(SRC_DIR) $(HEADER_DIR) | head -1 | sed -E 's/.*KERNEL_VERSION\s*=\s*"([^"]*)".*/\1/')
ISO_NAME := GatOS-$(KERNEL_VERSION).iso

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

# Build a standalone EFI executable (grub) that embeds your grub.cfg
# This creates $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI which UEFI firmware will look for on removable media
$(UEFI_GRUB): $(ISO_DIR)/boot/grub/grub.cfg
	@mkdir -p $(@D)
	# embed the grub.cfg from the ISO tree into a standalone EFI binary
	grub-mkstandalone --format=x86_64-efi --output=$@ \
		--locales="" --fonts="" \
		"boot/grub/grub.cfg=$(ISO_DIR)/boot/grub/grub.cfg"

# Generate ISO image (BIOS + UEFI hybrid) with versioned name
.PHONY: iso
iso: build $(UEFI_GRUB)
	@mkdir -p $(ISO_DIR)/boot
	cp $(DIST_DIR)/kernel.bin $(ISO_DIR)/boot/kernel.bin
	grub-mkrescue -o $(DIST_DIR)/$(ISO_NAME) $(ISO_DIR) || \
	grub-mkrescue -d /usr/lib/grub/i386-pc -o $(DIST_DIR)/$(ISO_NAME) $(ISO_DIR);

# Clean all build and dist files
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR) dist $(ISO_DIR)/boot/kernel.bin $(UEFI_GRUB)