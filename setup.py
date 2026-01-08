#!/usr/bin/env python3

import sys
import os
import shutil
import hashlib
import zipfile
import platform
import urllib.request
import re
import subprocess
from pathlib import Path
from time import time

# Configuration
ROOT_DIR = Path(__file__).parent.resolve()
TOOLCHAIN_DIR = ROOT_DIR / "toolchain"
SRC_DIR = ROOT_DIR / "src"

# URLs and Hashes
CONFIG = {
    "linux": {
        "url": "https://github.com/ApparentlyPlus/GatOS/releases/download/build-toolchain/x86_64-linux.zip",
        "hash": "c4e3da15dc6b5bdc74141b58b7da901af8485d0c39bc3463f5080c8487b0065e",
        "folder_name": "x86_64-linux"
    },
    "darwin": {
        "url": "https://github.com/ApparentlyPlus/GatOS/releases/download/build-toolchain/x86_64-macOS.zip",
        "hash": "1a35a1cb6169f684ae90c3d1841c40351767bce763c9bafbda66b192e3b45a1a",
        "folder_name": "x86_64-macos"
    },
    "win32": {
        "url": "https://github.com/ApparentlyPlus/GatOS/releases/download/build-toolchain/x86_64-win.zip",
        "hash": "82197d1a8ad5ac725012d1f7ffdf01e6dbb3573703b3f7882bec9c8d63728333",
        "folder_name": "x86_64-win"
    }
}

# ANSI Colors
if os.name == 'nt':
    os.system("color")

class Colors:
    CYAN = '\033[96m'
    MAGENTA = '\033[95m'
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    RESET = '\033[0m'
    BLACK_BG = '\033[40m'

# Helper Functions

def get_kernel_version() -> str:
    pattern = re.compile(r'KERNEL_VERSION\s*=\s*"([^"]*)"')
    
    if not SRC_DIR.exists(): 
        return "v0.0.0-unknown"
    
    for file in SRC_DIR.rglob("*.[chS]"):
        try:
            text = file.read_text(errors="ignore")
            match = pattern.search(text)
            if match:
                return match.group(1)
        except Exception:
            pass
            
    return "v0.0.0-unknown"

def print_banner():
    version = get_kernel_version()
    
    # Cyan Text on Black Background

    print(Colors.CYAN)
    print(f"   █████████           █████       ███████     █████████ ")
    print(f"  ███░░░░░███         ░░███      ███░░░░░███  ███░░░░░███")
    print(f" ███     ░░░  ██████  ███████   ███     ░░███░███    ░░░")
    print(f"░███         ░░░░░███░░░███░   ░███      ░███░░█████████")
    print(f"░███    █████ ███████  ░███    ░███      ░███ ░░░░░░░░███")
    print(f"░░███  ░░███ ███░░███  ░███ ███░░███     ███  ███    ░███")
    print(f" ░░█████████░░████████ ░░█████  ░░░███████░  ░░█████████")
    print(f"  ░░░░░░░░░  ░░░░░░░░   ░░░░░     ░░░░░░░     ░░░░░░░░░ ")
    print(Colors.RESET)

    # Magenta Text on Black Background
    banner_text = f">   GatOS Kernel {version} - Toolchain Setup Script   <"
    print(f"{Colors.MAGENTA}{banner_text}{Colors.RESET}")
    print("_" * len(banner_text) + "\n")

def get_platform_config():
    sys_plat = sys.platform
    if sys_plat.startswith("win"):
        return CONFIG["win32"], "win"
    elif sys_plat.startswith("linux"):
        return CONFIG["linux"], "linux"
    elif sys_plat == "darwin":
        return CONFIG["darwin"], "macos"
    else:
        print(f"{Colors.RED}[FATAL] Unsupported OS: {sys_plat}{Colors.RESET}")
        sys.exit(1)

def report_progress(block_num, block_size, total_size):
    downloaded = block_num * block_size
    if total_size > 0:
        percent = min(100, int(downloaded * 100 / total_size))
        bar_length = 40
        filled_length = int(bar_length * percent // 100)
        bar = '=' * filled_length + ' ' * (bar_length - filled_length)
        
        # Convert bytes to MB
        downloaded_mb = downloaded / (1024 * 1024)
        total_mb = total_size / (1024 * 1024)
        
        sys.stdout.write(f"\r{Colors.YELLOW}[DOWN] |{bar}| {percent}% ({downloaded_mb:.2f}/{total_mb:.2f} MB){Colors.RESET}")
        sys.stdout.flush()

def calculate_sha256(file_path):
    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        # Read and update hash string value in blocks of 4K
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    return sha256_hash.hexdigest()

def extract_toolchain(zip_path, extract_to):
    print(f"\n{Colors.YELLOW}[INFO] Extracting toolchain...{Colors.RESET}")
    try:
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(extract_to)
        print(f"{Colors.GREEN}[DONE] Extraction complete.{Colors.RESET}")
    except zipfile.BadZipFile:
        print(f"{Colors.RED}[FATAL] The downloaded file is corrupted.{Colors.RESET}")
        sys.exit(1)

def fix_mac_quarantine(folder):
    if sys.platform != "darwin":
        return

    print(f"{Colors.YELLOW}[INFO] MacOS Detected: Analyzing binaries and fixing Gatekeeper...{Colors.RESET}")
    
    target_path = Path(folder)
    
    # Mach-O Magic Bytes (covers 32/64 bit LE/BE and Universal binaries)
    # This identifies executables AND dylibs (dependencies)
    MACHO_MAGIC = {
        b'\xfe\xed\xfa\xce', b'\xfe\xed\xfa\xcf',
        b'\xce\xfa\xed\xfe', b'\xcf\xfa\xed\xfe',
        b'\xca\xfe\xba\xbe'
    }

    # State for sudo persistence
    use_sudo = False

    def run_secure(cmd_list, file_desc):
        nonlocal use_sudo
        
        # Prepare command
        cmd = ["sudo"] + cmd_list if use_sudo else cmd_list
        
        # Run command
        res = subprocess.run(cmd, capture_output=True, text=True)
        
        # Check for permission denied
        if res.returncode != 0 and "Permission denied" in res.stderr:
            if not use_sudo:
                print(f"{Colors.YELLOW}[WARN] Permission denied while processing {file_desc}.{Colors.RESET}")
                choice = input(f"{Colors.CYAN}    > The script needs 'sudo' to fix this. Allow? (y/N): {Colors.RESET}").strip().lower()
                
                if choice == 'y':
                    use_sudo = True
                    # Retry with sudo
                    cmd = ["sudo"] + cmd_list
                    res = subprocess.run(cmd, capture_output=True, text=True)
                else:
                    print(f"{Colors.RED}[ERR] Skipping {file_desc} (No permission).{Colors.RESET}")
                    return res
            else:
                # Failed even with sudo
                return res
        
        return res

    count = 0
    # Recursive walk
    for file_path in target_path.rglob("*"):
        if not file_path.is_file() or file_path.is_symlink():
            continue

        # Check if file is a Mach-O binary or dylib
        is_binary = False
        try:
            with open(file_path, "rb") as f:
                header = f.read(4)
                if header in MACHO_MAGIC:
                    is_binary = True
        except PermissionError:
            # If we can't read it, we likely need sudo to handle it anyway
            is_binary = True 
        except Exception:
            continue

        if is_binary:
            # Remove Quarantine
            # We explicitly ignore "No such xattr" errors (return code != 0 but typical stderr)
            # We ONLY care if it failed due to permissions, which run_secure handles.
            run_secure(["xattr", "-d", "com.apple.quarantine", str(file_path)], file_path.name)
            
            # Ad-hoc Sign
            # codesign -f -s - <file>
            res_sign = run_secure(["codesign", "--force", "--sign", "-", str(file_path)], file_path.name)
            
            if res_sign.returncode != 0:
                # Log actual signing errors
                print(f"{Colors.RED}[FAIL] Signing error on {file_path.name}: {res_sign.stderr.strip()}{Colors.RESET}")
            else:
                count += 1

    print(f"{Colors.GREEN}[DONE] Security patches applied to {count} binaries/libs.{Colors.RESET}")

def verify_tools_exist(toolchain_root, os_key):
    exe_ext = ".exe" if os_key == "win" else ""
    
    platform_dir = None
    if os_key == "win": platform_dir = toolchain_root / "x86_64-win"
    elif os_key == "linux": platform_dir = toolchain_root / "x86_64-linux"
    elif os_key == "macos": platform_dir = toolchain_root / "x86_64-macos"

    if not platform_dir.exists():
        return False

    required_tools = [
        platform_dir / "gcc/bin" / f"x86_64-elf-gcc{exe_ext}",
        platform_dir / "gcc/bin" / f"x86_64-elf-ld{exe_ext}",
        platform_dir / "grub" / f"grub-mkstandalone{exe_ext}",
        platform_dir / "grub" / f"grub-mkrescue{exe_ext}",
    ]

    # QEMU check
    if os_key == "win":
        required_tools.append(platform_dir / "qemu" / f"qemu-system-x86_64{exe_ext}")
    elif os_key == "linux":
        required_tools.append(platform_dir / "qemu" / "QEMU-x86_64.AppImage")
    elif os_key == "macos":
         required_tools.append(platform_dir / "qemu" / "bin" / "qemu-system-x86_64")

    missing = [t for t in required_tools if not t.exists()]
    
    if missing:
        print(f"{Colors.RED}[ERR] Missing tools:{Colors.RESET}")
        for m in missing:
            print(f" - {m}")
        return False
    
    return True

# Main Logic

def main():
    if sys.stdout.encoding != 'utf-8':
        sys.stdout.reconfigure(encoding='utf-8')
        
    print_banner()

    #OS Detection
    config, os_key = get_platform_config()
    print(f"{Colors.GREEN}[INFO] Detected OS: {os_key.upper()}{Colors.RESET}")

    # Directory Management
    should_download = True
    
    if TOOLCHAIN_DIR.exists():
        # Check if it looks populated
        if verify_tools_exist(TOOLCHAIN_DIR, os_key):
            print(f"{Colors.YELLOW}[INFO] Toolchain directory exists and looks valid.{Colors.RESET}")
            res = input(f"{Colors.CYAN}Do you want to redownload and repair it? (y/N): {Colors.RESET}").lower().strip()
            if res != 'y':
                should_download = False
                print(f"{Colors.GREEN}[DONE] Using existing toolchain.{Colors.RESET}")
            else:
                print(f"{Colors.YELLOW}[INFO] Cleaning existing toolchain...{Colors.RESET}")
                shutil.rmtree(TOOLCHAIN_DIR)
                TOOLCHAIN_DIR.mkdir()
        else:
             print(f"{Colors.YELLOW}[WARN] Toolchain directory exists but seems incomplete.{Colors.RESET}")
             TOOLCHAIN_DIR.mkdir(parents=True, exist_ok=True)
    else:
        TOOLCHAIN_DIR.mkdir(parents=True, exist_ok=True)

    if should_download:
        zip_name = "toolchain_temp.zip"
        zip_path = TOOLCHAIN_DIR / zip_name

        # Download
        print(f"{Colors.YELLOW}[INFO] Downloading toolchain, please hang tight :D{Colors.RESET}")
        try:
            urllib.request.urlretrieve(config['url'], zip_path, report_progress)
            sys.stdout.write("\n")
        except Exception as e:
            print(f"\n{Colors.RED}[FATAL] Download failed: {e}{Colors.RESET}")
            sys.exit(1)

        # 4. Verify Hash
        print(f"{Colors.YELLOW}[INFO] Verifying SHA256 hash...{Colors.RESET}")
        file_hash = calculate_sha256(zip_path)
        
        if file_hash != config['hash']:
            print(f"{Colors.RED}[FATAL] Hash mismatch!{Colors.RESET}")
            print(f"Expected: {config['hash']}")
            print(f"Got:      {file_hash}")
            print("Possible corrupted download or MITM attack.")
            zip_path.unlink()
            sys.exit(1)
        else:
             print(f"{Colors.GREEN}[PASS] Checksum verified.{Colors.RESET}")

        # Extract
        extract_toolchain(zip_path, TOOLCHAIN_DIR)

        # Cleanup
        print(f"{Colors.YELLOW}[INFO] Cleaning up zip file...{Colors.RESET}")
        if zip_path.exists():
            zip_path.unlink()

        # Post-Install Fixes
        if sys.platform != "win32":
            # Make binaries executable
            subprocess.run(["chmod", "-R", "+x", str(TOOLCHAIN_DIR)], stderr=subprocess.DEVNULL)
            # Fix macOS Gatekeeper
            fix_mac_quarantine(TOOLCHAIN_DIR)

    # Final Verification
    print(f"{Colors.YELLOW}[INFO] validating installation...{Colors.RESET}")
    if verify_tools_exist(TOOLCHAIN_DIR, os_key):
        print(f"\n{Colors.GREEN}[SUCCESS] Toolchain setup complete! You can now run 'python run.py'.{Colors.RESET}\n")
    else:
        print(f"\n{Colors.RED}[FAIL] Setup completed, but tools are missing. Check logs.{Colors.RESET}\n")
        sys.exit(1)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n{Colors.RED}[ABORT] Setup cancelled by user.{Colors.RESET}")
        sys.exit(1)
