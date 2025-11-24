#!/usr/bin/env python3

import os
import re
import sys
import shutil
import argparse
import subprocess
from pathlib import Path
from typing import List, Dict, Optional, Tuple
from multiprocessing import Pool, cpu_count

# Configuration & Constants

if os.name == 'nt':
    os.system("color")

NC = '\033[0m'
GREEN = '\033[1;32m'
RED = '\033[1;31m'
YELLOW = '\033[1;33m'
BLUE = '\033[1;34m'

if os.name == 'nt':
    OS_NAME = "win"
    EXE_EXT = ".exe"
elif sys.platform == "linux":
    OS_NAME = "linux"
    EXE_EXT = ""
elif sys.platform == "darwin":
    OS_NAME = "macos"
    EXE_EXT = ""
else:
    sys.stderr.write(f"{RED}[FATAL] Unsupported OS: {sys.platform}{NC}\n")
    sys.exit(1)

ROOT_DIR = Path(__file__).parent.resolve()
BASE_TOOLCHAIN_DIR = ROOT_DIR / "toolchain"
PLATFORM_TOOLCHAIN_DIR = BASE_TOOLCHAIN_DIR / f"x86_64-{OS_NAME}"
GRUB_DIR = PLATFORM_TOOLCHAIN_DIR / "grub"

SRC_DIR = ROOT_DIR / "src/impl"
HEADER_DIR = ROOT_DIR / "src/headers"
BUILD_DIR = ROOT_DIR / "build"
DIST_DIR = ROOT_DIR / "dist/x86_64"
ISO_DIR = ROOT_DIR / "targets/x86_64/iso"

UEFI_DIR = ISO_DIR / "EFI/BOOT"
UEFI_GRUB = UEFI_DIR / "BOOTX64.EFI"
GRUB_CFG = ISO_DIR / "boot/grub/grub.cfg"
KERNEL_BIN = DIST_DIR / "kernel.bin"
DEBUG_LOG = ROOT_DIR / "debug.log"

# Toolchain Paths

if not PLATFORM_TOOLCHAIN_DIR.exists():
    sys.stderr.write(f"{RED}[FATAL] Toolchain not found at: {PLATFORM_TOOLCHAIN_DIR}\n")
    sys.stderr.write(f"{YELLOW}Please run 'python setup.py' to install the toolchain.{NC}\n")
    sys.exit(1)

CC = PLATFORM_TOOLCHAIN_DIR / "gcc" / "bin" / f"x86_64-elf-gcc{EXE_EXT}"
LD = PLATFORM_TOOLCHAIN_DIR / "gcc" / "bin" / f"x86_64-elf-ld{EXE_EXT}"
GRUB_MKSTANDALONE = GRUB_DIR / f"grub-mkstandalone{EXE_EXT}"
GRUB_MKRESCUE_CMD = GRUB_DIR / f"grub-mkrescue{EXE_EXT}"

if OS_NAME == "win":
    QEMU_EXEC = PLATFORM_TOOLCHAIN_DIR / "qemu" / f"qemu-system-x86_64{EXE_EXT}"
    XORRISO_EXEC = None
    GRUB_MODULE_DIR = GRUB_DIR / "x86_64-efi"
    GRUB_FONT_PATH = None
elif OS_NAME == "linux":
    QEMU_EXEC = PLATFORM_TOOLCHAIN_DIR / "qemu" / "QEMU-x86_64.AppImage"
    XORRISO_EXEC = PLATFORM_TOOLCHAIN_DIR / "xorriso" / "xorriso"
    GRUB_MODULE_DIR = GRUB_DIR / "x86_64-efi"
    GRUB_FONT_PATH = GRUB_DIR / "unicode.pf2"
elif OS_NAME == "macos":
    QEMU_EXEC = PLATFORM_TOOLCHAIN_DIR / "qemu" / "bin" / "qemu-system-x86_64"
    XORRISO_EXEC = PLATFORM_TOOLCHAIN_DIR / "xorriso" / "xorriso"
    GRUB_MODULE_DIR = GRUB_DIR / "x86_64-efi"
    GRUB_FONT_PATH = GRUB_DIR / "unicode.pf2"

# Compiler Flags & Profiles

CFLAGS_BASE = ["-m64", "-ffreestanding", "-nostdlib", "-fno-pic", "-mcmodel=kernel", f"-I{HEADER_DIR}"]
CPPFLAGS = [f"-I{HEADER_DIR}", "-D__ASSEMBLER__"]
LDFLAGS = ["-n", "-nostdlib", f"-T{ROOT_DIR / 'targets/x86_64/linker.ld'}", "--no-relax", "-g"]

# Optimization Levels
CFLAGS_FAST = ["-O2", "-fomit-frame-pointer", "-fpredictive-commoning", "-fstrict-aliasing"]
CFLAGS_VFAST = ["-O3", "-funroll-loops", "-fomit-frame-pointer", "-fno-stack-protector", "-fstrict-aliasing"]

# Profile Definitions
BUILD_PROFILES = {
    "default": {
        "flags": [], 
        "confirm": False
    },
    "fast": {
        "flags": CFLAGS_FAST, 
        "confirm": False
    },
    "vfast": {
        "flags": CFLAGS_VFAST, 
        "confirm": True,
        "msg": "WARNING: 'vfast' uses aggressive optimizations (-O3, -funroll-loops) which may cause unexpected kernel behavior or instability."
    }
}

# Core Functions

def run_cmd(cmd: List[str | Path], cwd: Optional[Path] = None, env: Optional[Dict] = None, check: bool = True) -> bool:
    cmd_str = [str(c) for c in cmd]
    print(f"{BLUE}>>> {' '.join(cmd_str)}{f' (in {cwd})' if cwd else ''}{NC}")
    run_env = os.environ.copy()
    if env: run_env.update(env)
    try:
        subprocess.run(cmd_str, cwd=cwd, env=run_env, check=check, text=True)
        return True
    except subprocess.CalledProcessError as e:
        sys.stderr.write(f"{RED}[ERROR] Command failed with exit code {e.returncode}{NC}\n")
        if check: sys.exit(e.returncode)
        return False
    except FileNotFoundError as e:
        sys.stderr.write(f"{RED}[FATAL] Executable not found: {cmd_str[0]}{NC}\n")
        sys.exit(1)

def get_kernel_version() -> str:
    pattern = re.compile(r'KERNEL_VERSION\s*=\s*"([^"]*)"')
    for directory in (SRC_DIR, HEADER_DIR):
        for file in directory.rglob("*.[chS]"):
            try:
                match = pattern.search(file.read_text(errors="ignore"))
                if match: return match.group(1)
            except Exception: pass
    return "v0.0.0-unknown"

def fix_unix_permissions():
    if OS_NAME == "win": return
    print(f"{YELLOW}[INFO] Ensuring toolchain permissions...{NC}")
    subprocess.run(["chmod", "-R", "+x", str(BASE_TOOLCHAIN_DIR)], check=False, capture_output=True)

def compile_worker(job):
    compiler, src, obj, flags = job
    obj.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(compiler), "-c", *flags, str(src), "-o", str(obj)]
    result = subprocess.run(cmd, text=True, capture_output=True)
    return f"{RED}[FAIL] {src.name}:{NC}\n{result.stderr}" if result.returncode != 0 else f"{BLUE}[OK] {src.name}{NC}"

def compile_sources(c_files: List[Path], asm_files: List[Path], profile_name: str) -> bool:
    profile = BUILD_PROFILES.get(profile_name, BUILD_PROFILES["default"])
    
    if profile.get("confirm", False):
        print(f"{RED}{profile['msg']}{NC}")
        try:
            sys.stdout.flush()
            response = input(f"{YELLOW}Do you want to proceed? (y/N): {NC}").strip().lower()
            if response != 'y':
                print(f"{RED}[ABORT] Build cancelled by user.{NC}")
                sys.exit(0)
        except KeyboardInterrupt:
            print(f"\n{RED}[ABORT] Build cancelled.{NC}")
            sys.exit(0)

    final_cflags = CFLAGS_BASE + profile["flags"]
    
    print(f"{YELLOW}[INFO] Starting parallel compilation (Profile: {profile_name.upper()})...{NC}")
    
    jobs = []
    for src in c_files:
        jobs.append((CC, src, BUILD_DIR / src.relative_to(SRC_DIR).with_suffix(".o"), final_cflags))
    for src in asm_files:
        jobs.append((CC, src, BUILD_DIR / src.relative_to(SRC_DIR).with_suffix(".o"), CPPFLAGS))

    with Pool(processes=cpu_count()) as pool:
        results = pool.map(compile_worker, jobs)
        pool.close()
        pool.join()

    errors = [r for r in results if "[FAIL]" in r]
    for res in results:
        if "[FAIL]" in res: sys.stderr.write(res + "\n")

    if errors:
        sys.stderr.write(f"{RED}[FATAL] Compilation failed for {len(errors)} files.{NC}\n")
        sys.exit(1)
    print(f"{GREEN}[INFO] Compilation successful.{NC}")
    return True

def link_kernel(obj_files: List[Path]):
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    run_cmd([LD, *LDFLAGS, "-o", KERNEL_BIN] + [str(f) for f in obj_files])

def make_uefi_grub():
    UEFI_DIR.mkdir(parents=True, exist_ok=True)
    run_cmd([GRUB_MKSTANDALONE, f"--directory={GRUB_MODULE_DIR}", "--format=x86_64-efi", f"--output={UEFI_GRUB}", "--locales=", "--fonts=", f"boot/grub/grub.cfg={GRUB_CFG}"])

def make_iso(output_iso: Path):
    (ISO_DIR / "boot").mkdir(parents=True, exist_ok=True)
    shutil.copy2(KERNEL_BIN, ISO_DIR / "boot/kernel.bin")
    print(f"{YELLOW}[INFO] Creating hybrid ISO: {output_iso}{NC}")

    if OS_NAME in ["linux", "macos"]:
        if not GRUB_FONT_PATH.exists():
            sys.stderr.write(f"{RED}[FATAL] Unicode font missing at {GRUB_FONT_PATH}{NC}\n")
            sys.exit(1)
            
        cmd = [
            "./grub-mkrescue",
            f"--xorriso={XORRISO_EXEC}",
            "--fonts=unicode",
            "--themes=",
            "-o", str(output_iso),
            str(ISO_DIR)
        ]
        # Runs inside GRUB_DIR to satisfy internal relative paths on macOS/Linux
        run_cmd(cmd, cwd=GRUB_DIR)
        
    else:
        # Windows Logic (Absolute paths, C++ wrapper)
        if not GRUB_MKRESCUE_CMD.exists():
            sys.stderr.write(f"{RED}[FATAL] grub-mkrescue wrapper not found at: {GRUB_MKRESCUE_CMD}{NC}\n")
            sys.exit(1)
        
        cmd = [
            str(GRUB_MKRESCUE_CMD.resolve()), 
            "-d", str(GRUB_DIR.resolve()), 
            "-o", str(output_iso.resolve()), 
            str(ISO_DIR.resolve())
        ]
        run_cmd(cmd, cwd=GRUB_DIR, check=True)

def build_iso(c_src: List[Path], asm_src: List[Path], obj_files: List[Path], iso_name: str, profile: str):
    if compile_sources(c_src, asm_src, profile):
        link_kernel(obj_files)
        make_uefi_grub()
        make_iso(DIST_DIR / iso_name)

def clean():
    print(f"{YELLOW}[INFO] Cleaning build artifacts...{NC}")
    for p in (BUILD_DIR, DIST_DIR.parent):
        if p.exists(): shutil.rmtree(p)
    if (ISO_DIR / "boot/kernel.bin").exists(): (ISO_DIR / "boot/kernel.bin").unlink()
    if UEFI_GRUB.exists(): UEFI_GRUB.unlink()
    if UEFI_DIR.exists() and not any(UEFI_DIR.iterdir()): UEFI_DIR.rmdir()
    if DEBUG_LOG.exists(): DEBUG_LOG.unlink()
    print(f"{GREEN}[INFO] Clean complete.{NC}")

def verify_environment() -> bool:
    print(f"{YELLOW}[INFO] Verifying environment...{NC}")
    fix_unix_permissions()
    missing = []
    tools = {"QEMU": QEMU_EXEC, "GCC": CC, "LD": LD, "GRUB Standalone": GRUB_MKSTANDALONE, "GRUB Rescue": GRUB_MKRESCUE_CMD}
    if OS_NAME in ["linux", "macos"]: tools["Xorriso"] = XORRISO_EXEC
    for name, path in tools.items():
        if path and not path.exists(): missing.append(f"{name} ({path})")
    if missing:
        sys.stderr.write(f"{RED}[ERROR] Missing dependencies:{NC}\n")
        for m in missing: sys.stderr.write(f"- {m}\n")
        return False
    print(f"{GREEN}[INFO] Environment OK.{NC}")
    return True

def find_iso_file() -> Optional[Path]:
    if not DIST_DIR.exists(): return None
    isos = list(DIST_DIR.glob("GatOS-*.iso"))
    if not isos: return None
    isos.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return isos[0]

def run_qemu(iso_file: Path):
    print(f"{GREEN}[SUCCESS] Starting QEMU with {iso_file.name}...{NC}")
    qemu_cmd = [str(QEMU_EXEC)]
    if OS_NAME == "linux": qemu_cmd.append("qemu-system-x86_64")
    run_cmd([*qemu_cmd, "-cdrom", str(iso_file), "-serial", "mon:stdio", "-serial", f"file:{DEBUG_LOG}"], check=False)

# Entry Point

def main():
    parser = argparse.ArgumentParser(description="GatOS Build System")
    parser.add_argument("args", nargs="*", help="Command (clean, build, all) and/or Profile (fast, vfast)")
    args_parsed = parser.parse_args()
    user_args = args_parsed.args

    command = "all"
    profile = "default"

    valid_commands = {"all", "build", "clean"}
    valid_profiles = set(BUILD_PROFILES.keys())

    for arg in user_args:
        if arg in valid_commands:
            command = arg
        elif arg in valid_profiles:
            profile = arg
        else:
            print(f"{YELLOW}[WARN] Unknown argument '{arg}', ignoring.{NC}")

    c_src = list(SRC_DIR.rglob("*.c"))
    asm_src = list(SRC_DIR.rglob("*.S"))
    obj_files = [BUILD_DIR / f.relative_to(SRC_DIR).with_suffix(".o") for f in c_src + asm_src]
    iso_name = f"GatOS-{get_kernel_version()}.iso"

    if command == "clean":
        clean()
        return

    if not verify_environment(): sys.exit(1)

    if command == "build":
        clean()
        build_iso(c_src, asm_src, obj_files, iso_name, profile)
    elif command == "all":
        try: clean()
        except Exception: pass
        build_iso(c_src, asm_src, obj_files, iso_name, profile)
        iso = find_iso_file()
        if iso: run_qemu(iso)
        else:
            sys.stderr.write(f"{RED}[ERROR] ISO file not found after build.{NC}\n")
            sys.exit(1)

if __name__ == "__main__":
    main()
