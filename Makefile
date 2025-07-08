# Toolchain configuration
CC := x86_64-elf-gcc
AS := x86_64-elf-as
LD := x86_64-elf-ld

# Compilation flags
CFLAGS := -I src/headers -ffreestanding
ASFLAGS := --64
LDFLAGS := -n -T targets/x86_64/linker.ld

# Build directories
BUILD_DIR := build
DIST_DIR := dist
ISO_DIR := targets/x86_64/iso

# Kernel source files
KERNEL_SRC_DIR := src/impl/kernel
KERNEL_SRC_FILES := $(shell find $(KERNEL_SRC_DIR) -name '*.c')
KERNEL_OBJ_FILES := $(patsubst $(KERNEL_SRC_DIR)/%.c,$(BUILD_DIR)/kernel/%.o,$(KERNEL_SRC_FILES))

# x86_64 source files
X86_64_SRC_DIR := src/impl/x86_64
X86_64_C_SRC_FILES := $(shell find $(X86_64_SRC_DIR) -name '*.c')
X86_64_ASM_SRC_FILES := $(shell find $(X86_64_SRC_DIR) -name '*.S')
X86_64_C_OBJ_FILES := $(patsubst $(X86_64_SRC_DIR)/%.c,$(BUILD_DIR)/x86_64/%.o,$(X86_64_C_SRC_FILES))
X86_64_ASM_OBJ_FILES := $(patsubst $(X86_64_SRC_DIR)/%.S,$(BUILD_DIR)/x86_64/%.o,$(X86_64_ASM_SRC_FILES))
X86_64_OBJ_FILES := $(X86_64_C_OBJ_FILES) $(X86_64_ASM_OBJ_FILES)

# Default target
.PHONY: all
all: build-x86_64

# Build kernel objects
$(BUILD_DIR)/kernel/%.o: $(KERNEL_SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

# Build x86_64 C objects
$(BUILD_DIR)/x86_64/%.o: $(X86_64_SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

# Build x86_64 assembly objects
$(BUILD_DIR)/x86_64/%.o: $(X86_64_SRC_DIR)/%.S
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) $< -o $@

# Main build target
.PHONY: build-x86_64
build-x86_64: $(KERNEL_OBJ_FILES) $(X86_64_OBJ_FILES)
	@mkdir -p $(DIST_DIR)/x86_64
	$(LD) $(LDFLAGS) -o $(DIST_DIR)/x86_64/kernel.bin $^
	@cp $(DIST_DIR)/x86_64/kernel.bin $(ISO_DIR)/boot/kernel.bin
	grub-mkrescue /usr/lib/grub/i386-pc -o $(DIST_DIR)/x86_64/kernel.iso $(ISO_DIR)

# Clean build artifacts
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR) $(ISO_DIR)/boot/kernel.bin