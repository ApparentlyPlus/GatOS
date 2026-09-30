# x86_64 Static Toolchain Build Resources

This directory contains the experimental scripts, build notes, and source modifications used to generate the static x86_64 toolchain for **GatOS**.

### Disclaimer: Unsupported & Experimental

**Use the prebuilt portable toolchain unless you have a specific, critical reason to do otherwise.**

> [!CAUTION]
> This is strictly for educational purposes and transparency.
> As stated in the main project documentation: These scripts are not guaranteed to work, are not tested beyond the original author's machine, and will not be maintained. Attempting to rebuild the static toolchain from source is a fragile, complex process that requires significant manual intervention.


### Host Environment

All scripts and instructions in this directory assume the following host environment:

* **OS:** Linux
* **Distro:** Debian (Trixie/Testing)
* **Architecture:** x86_64

**Note:** The macOS and Windows targets are cross-compiled from this Linux host. Do not attempt to run the macOS scripts on a Mac or the Windows notes on Windows; they are designed to run on the Debian host to produce binaries for those platforms.

### File structure conventions

* **`.sh` files:** Bash scripts attempting to automate the build process for specific components. Check the contents just to be sure, but you can usually run these right out the box.
* **`.txt` files:** Raw notes, URLs, or manual step-by-step instructions for components where automation was not feasible or where pre-built static binaries were fetched and patched.

### Target Breakdowns

#### Linux

Contains instructions for building the native Linux toolchain.

* Includes build scripts for `mtools` and `xorriso`.
* Includes fetch instructions for `gcc`, `qemu`, and `grub`.

#### macOS

Contains **cross-compilation** scripts (building macOS binaries on Linux).

> [!NOTE]
> For the `osxcross` build, I used the `macOS 14 SDK`, which you can pull using:
> ```
> curl -Lo tarballs/MacOSX14.5.sdk.tar.xz https://github.com/joseluisq/macosx-sdks/releases/download/14.5/MacOSX14.5.sdk.tar.xz
> ```

* Heavily dependent on a working cross compiler (`osxcross`). You need to clone the `osxcross` repo, build it from source and have it in be in your desktop (or tweak the default path in the build scripts).
* Includes scripts to build `gcc`, `mtools`, `qemu`, and `xorriso` specifically for macOS execution.
* All macOS builds target both Intel and Arm, then merge the binaries using `lipo` to create Mach-O universal binaries.

#### Windows

Contains **cross-compilation** notes and fetch instructions (building/getting Windows binaries on Linux).

* **`grub-mkrescue.cpp`**: A custom C++ wrapper used to replace the standard `grub-mkrescue` utility, which is absent for windows.
* Includes notes for obtaining `gcc`, `grub`, `mtools`, `qemu`, and `xorriso` for Windows.

### Licenses and upstream sources

The toolchain is assembled from these upstream projects. Each keeps its own license, and the scripts, patches and notes I used to build or modify them are in this directory.

| Component | Where it comes from | License |
|---|---|---|
| GCC and GNU Binutils (`x86_64-elf`) | [lordmilko/i686-elf-tools](https://github.com/lordmilko/i686-elf-tools) on Linux and Windows. Built here with [osxcross](https://github.com/tpoechtrager/osxcross) and the [macOS 14.5 SDK](https://github.com/joseluisq/macosx-sdks) on macOS | GPL-3.0-or-later (GCC also has the Runtime Library Exception) |
| GRUB 2.14 | [ftp.gnu.org/gnu/grub](https://ftp.gnu.org/gnu/grub/) (`grub-2.14-for-windows` on Windows, with the EFI modules rebuilt). Built from source on Linux and macOS, with the patches in `Linux/linux-build-grub.txt` and `macOS/macos-build-grub.txt` | GPL-3.0-or-later |
| `grub-mkrescue` for Windows | `Windows/grub-mkrescue.cpp`, a port of GRUB's `util/grub-mkrescue.c` | GPL-3.0-or-later |
| QEMU | [lucasmz1/Qemu-AppImage](https://github.com/lucasmz1/Qemu-AppImage) on Linux, [qemu.org](https://www.qemu.org/download/#windows) on Windows, built from the Homebrew dependency tree on macOS | GPL-2.0, with some parts under other licenses (see upstream) |
| xorriso 1.5.2 | [ftp.gnu.org/gnu/xorriso](https://ftp.gnu.org/gnu/xorriso/) on Linux and macOS, [PeyTy/xorriso-exe-for-windows](https://github.com/PeyTy/xorriso-exe-for-windows) on Windows | GPL-3.0-or-later |
| mtools | [hamishcoleman/mtools](https://github.com/hamishcoleman/mtools) | GPL-3.0-or-later |

### Usage

There is no master makefile. To use these:

1. Read the specific `.txt` or `.sh` file for the component you are interested in.
2. Manually satisfy all dependencies (headers, cross-compilers).
3. Execute or follow the steps individually.