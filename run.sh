#!/usr/bin/env bash
set -euo pipefail

# Color definitions
readonly NC='\033[0m'
readonly GREEN='\033[1;32m'
readonly RED='\033[1;31m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[1;34m'

# Configuration
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly QEMU_EXEC="qemu-system-x86_64"
readonly ISO_PATH="dist/x86_64/kernel.iso"

function command_exists() {
  command -v "$1" >/dev/null 2>&1
}

function is_root() {
  [[ "$(id -u)" -eq 0 ]]
}

function run_make() {
  if [[ ! -f "${SCRIPT_DIR}/Makefile" ]]; then
    echo -e "${RED}[ERROR] Makefile not found in ${SCRIPT_DIR}${NC}" >&2
    return 1
  fi

  echo -e "${YELLOW}[INFO] Cleaning previous build...${NC}"
  if ! make clean; then
    echo -e "${YELLOW}[WARNING] make clean failed - continuing with build${NC}" >&2
  fi

  echo -e "${YELLOW}[INFO] Building GatOS...${NC}"
  if ! make; then
    echo -e "${RED}[ERROR] Build failed${NC}" >&2
    return 1
  fi

  return 0
}

function verify_environment() {
  local missing=()

  if ! command_exists make; then
    missing+=("make")
  fi

  if ! command_exists "${QEMU_EXEC}"; then
    missing+=("qemu-system-x86_64")
  fi

  if [[ ${#missing[@]} -gt 0 ]]; then
    echo -e "${RED}[ERROR] Missing dependencies: ${missing[*]}${NC}" >&2
    return 1
  fi

  return 0
}

function run_qemu() {
  if [[ ! -f "${SCRIPT_DIR}/${ISO_PATH}" ]]; then
    echo -e "${RED}[ERROR] Bootable ISO not found at ${ISO_PATH}${NC}" >&2
    return 1
  fi

  echo -e "${GREEN}[SUCCESS] Build complete. Starting QEMU...${NC}"
  "${QEMU_EXEC}" -cdrom "${SCRIPT_DIR}/${ISO_PATH}" -serial stdio
}

function main() {

  # Verify build environment first
  if ! verify_environment; then
    echo "Please run setup.sh to install the toolchain first! Don't forget to restart your terminal after!"
    exit 1
  fi

  # Attempt build
  if ! run_make; then
    echo "If you ran setup.sh, you probably forgot to restart your terminal."
    exit 1
  fi

  # Verify QEMU is available
  if ! command_exists "${QEMU_EXEC}"; then
    echo -e "${RED}[ERROR] QEMU is required but not found${NC}" >&2
    echo "If you ran setup.sh, you probably forgot to restart your terminal."
    exit 1
  fi

  # Run QEMU
  run_qemu
}

main "$@"