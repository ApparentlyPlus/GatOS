#!/usr/bin/env python3

import sys
import os
import re
import shutil
import hashlib
import zipfile
import subprocess
import urllib.request
from pathlib import Path

ROOT_DIR = Path(__file__).parent.resolve()
TOOLCHAIN_DIR = ROOT_DIR / "toolchain"
SRC_DIR = ROOT_DIR / "src"

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
        "hash": "9e395023e886c56f1d4d31b0d0a61f744890097d30596955b6bd7bb5a5200b03",
        "folder_name": "x86_64-win"
    }
}

if os.name == 'nt':
    os.system("color")

CYAN = '\033[96m'
MAGENTA = '\033[95m'
GREEN = '\033[92m'
RED = '\033[91m'
YELLOW = '\033[93m'
NC = '\033[0m'

def get_kernel_version():
    pattern = re.compile(r'KERNEL_VERSION\s*=\s*"([^"]*)"')
    if SRC_DIR.exists():
        for file in SRC_DIR.rglob("*.[chS]"):
            try:
                m = pattern.search(file.read_text(errors="ignore"))
                if m: return m.group(1)
            except Exception: pass
    return "v0.0.0-unknown"

def print_banner():
    version = get_kernel_version()

    print(CYAN)
    print("   █████████           █████       ███████     █████████ ")
    print("  ███░░░░░███         ░░███      ███░░░░░███  ███░░░░░███")
    print(" ███     ░░░  ██████  ███████   ███     ░░███░███    ░░░")
    print("░███         ░░░░░███░░░███░   ░███      ░███░░█████████")
    print("░███    █████ ███████  ░███    ░███      ░███ ░░░░░░░░███")
    print("░░███  ░░███ ███░░███  ░███ ███░░███     ███  ███    ░███")
    print(" ░░█████████░░████████ ░░█████  ░░░███████░  ░░█████████")
    print("  ░░░░░░░░░  ░░░░░░░░   ░░░░░     ░░░░░░░     ░░░░░░░░░ ")
    print(NC)

    banner = f">   GatOS Kernel {version} - Toolchain Setup Script   <"
    print(f"{MAGENTA}{banner}{NC}")
    print("_" * len(banner) + "\n")

def get_platform_config():
    plat = sys.platform
    if plat.startswith("win"):
        return CONFIG["win32"], "win"
    elif plat.startswith("linux"):
        return CONFIG["linux"], "linux"
    elif plat == "darwin":
        return CONFIG["darwin"], "macos"
    else:
        print(f"{RED}[FATAL] Unsupported OS: {plat}{NC}")
        sys.exit(1)

# urlretrieve reporthook
def progress(blocks, block_size, total):
    done = blocks * block_size
    if total > 0:
        pct = min(100, int(done * 100 / total))
        filled = 40 * pct // 100
        bar = '=' * filled + ' ' * (40 - filled)
        sys.stdout.write(f"\r{YELLOW}[DOWN] |{bar}| {pct}% ({done / 2**20:.2f}/{total / 2**20:.2f} MB){NC}")
        sys.stdout.flush()

def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            h.update(chunk)
    return h.hexdigest()

def extract_toolchain(zip_path, dest):
    print(f"\n{YELLOW}[INFO] Extracting toolchain...{NC}")
    try:
        with zipfile.ZipFile(zip_path, 'r') as z:
            z.extractall(dest)
        print(f"{GREEN}[DONE] Extraction complete.{NC}")
    except zipfile.BadZipFile:
        print(f"{RED}[FATAL] The downloaded file is corrupted.{NC}")
        sys.exit(1)

def fix_mac_quarantine(folder):
    if sys.platform != "darwin":
        return

    print(f"{YELLOW}[INFO] MacOS Detected: Analyzing binaries and fixing Gatekeeper...{NC}")

    # Mach-O magics (32/64 bit, LE/BE, universal). Matches executables and dylibs.
    MACHO = {
        b'\xfe\xed\xfa\xce', b'\xfe\xed\xfa\xcf',
        b'\xce\xfa\xed\xfe', b'\xcf\xfa\xed\xfe',
        b'\xca\xfe\xba\xbe'
    }

    use_sudo = False

    # Runs cmd, and asks once to switch to sudo if something is permission denied.
    def sh(cmd, desc):
        nonlocal use_sudo
        res = subprocess.run((["sudo"] if use_sudo else []) + cmd, capture_output=True, text=True)

        if res.returncode != 0 and "Permission denied" in res.stderr and not use_sudo:
            print(f"{YELLOW}[WARN] Permission denied while processing {desc}.{NC}")
            ans = input(f"{CYAN}    > The script needs 'sudo' to fix this. Allow? (y/N): {NC}").strip().lower()
            if ans == 'y':
                use_sudo = True
                res = subprocess.run(["sudo"] + cmd, capture_output=True, text=True)
            else:
                print(f"{RED}[ERR] Skipping {desc} (No permission).{NC}")
        return res

    count = 0
    for f in Path(folder).rglob("*"):
        if not f.is_file() or f.is_symlink():
            continue

        try:
            with open(f, "rb") as fh:
                is_macho = fh.read(4) in MACHO
        except PermissionError:
            is_macho = True  # can't read it, so it will need sudo anyway
        except Exception:
            continue

        if is_macho:
            # xattr fails with "No such xattr" on clean files, which is fine. Only permission errors matter.
            sh(["xattr", "-d", "com.apple.quarantine", str(f)], f.name)
            sign = sh(["codesign", "--force", "--sign", "-", str(f)], f.name)

            if sign.returncode != 0:
                print(f"{RED}[FAIL] Signing error on {f.name}: {sign.stderr.strip()}{NC}")
            else:
                count += 1

    print(f"{GREEN}[DONE] Security patches applied to {count} binaries/libs.{NC}")

def tools_ok(root, os_key):
    exe = ".exe" if os_key == "win" else ""
    plat_dir = root / {"win": "x86_64-win", "linux": "x86_64-linux", "macos": "x86_64-macos"}[os_key]

    if not plat_dir.exists():
        return False

    need = [
        plat_dir / "gcc/bin" / f"x86_64-elf-gcc{exe}",
        plat_dir / "gcc/bin" / f"x86_64-elf-ld{exe}",
        plat_dir / "grub" / f"grub-mkstandalone{exe}",
        plat_dir / "grub" / f"grub-mkrescue{exe}",
    ]
    if os_key == "win":
        need.append(plat_dir / "qemu" / f"qemu-system-x86_64{exe}")
    elif os_key == "linux":
        need.append(plat_dir / "qemu" / "QEMU-x86_64.AppImage")
    elif os_key == "macos":
        need.append(plat_dir / "qemu" / "bin" / "qemu-system-x86_64")

    missing = [t for t in need if not t.exists()]
    if missing:
        print(f"{RED}[ERR] Missing tools:{NC}")
        for m in missing:
            print(f" - {m}")
        return False
    return True

def main():
    if sys.stdout.encoding != 'utf-8':
        sys.stdout.reconfigure(encoding='utf-8')

    print_banner()

    config, os_key = get_platform_config()
    print(f"{GREEN}[INFO] Detected OS: {os_key.upper()}{NC}")

    download = True

    if TOOLCHAIN_DIR.exists():
        if tools_ok(TOOLCHAIN_DIR, os_key):
            print(f"{YELLOW}[INFO] Toolchain directory exists and looks valid.{NC}")
            ans = input(f"{CYAN}Do you want to redownload and repair it? (y/N): {NC}").lower().strip()
            if ans != 'y':
                download = False
                print(f"{GREEN}[DONE] Using existing toolchain.{NC}")
            else:
                print(f"{YELLOW}[INFO] Cleaning existing toolchain...{NC}")
                shutil.rmtree(TOOLCHAIN_DIR)
                TOOLCHAIN_DIR.mkdir()
        else:
            print(f"{YELLOW}[WARN] Toolchain directory exists but seems incomplete.{NC}")
            TOOLCHAIN_DIR.mkdir(parents=True, exist_ok=True)
    else:
        TOOLCHAIN_DIR.mkdir(parents=True, exist_ok=True)

    if download:
        zip_path = TOOLCHAIN_DIR / "toolchain_temp.zip"

        print(f"{YELLOW}[INFO] Downloading toolchain, please hang tight :D{NC}")
        try:
            urllib.request.urlretrieve(config['url'], zip_path, progress)
            sys.stdout.write("\n")
        except Exception as e:
            print(f"\n{RED}[FATAL] Download failed: {e}{NC}")
            sys.exit(1)

        print(f"{YELLOW}[INFO] Verifying SHA256 hash...{NC}")
        got = sha256_of(zip_path)

        if got != config['hash']:
            print(f"{RED}[FATAL] Hash mismatch!{NC}")
            print(f"Expected: {config['hash']}")
            print(f"Got:      {got}")
            print("Possible corrupted download or MITM attack.")
            zip_path.unlink()
            sys.exit(1)
        else:
            print(f"{GREEN}[PASS] Checksum verified.{NC}")

        extract_toolchain(zip_path, TOOLCHAIN_DIR)

        print(f"{YELLOW}[INFO] Cleaning up zip file...{NC}")
        if zip_path.exists():
            zip_path.unlink()

        if sys.platform != "win32":
            subprocess.run(["chmod", "-R", "+x", str(TOOLCHAIN_DIR)], stderr=subprocess.DEVNULL)
            fix_mac_quarantine(TOOLCHAIN_DIR)

    print(f"{YELLOW}[INFO] validating installation...{NC}")
    if tools_ok(TOOLCHAIN_DIR, os_key):
        print(f"\n{GREEN}[SUCCESS] Toolchain setup complete! You can now run 'python run.py'.{NC}\n")
    else:
        print(f"\n{RED}[FAIL] Setup completed, but tools are missing. Check logs.{NC}\n")
        sys.exit(1)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n{RED}[ABORT] Setup cancelled by user.{NC}")
        sys.exit(1)
