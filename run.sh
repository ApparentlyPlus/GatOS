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
readonly DIST_DIR="dist/x86_64"

function command_exists() {
  command -v "$1" >/dev/null 2>&1
}

function is_root() {
  [[ "$(id -u)" -eq 0 ]]
}

function find_iso_file() {
  local iso_pattern="${DIST_DIR}/GatOS-*.iso"
  local iso_files=($(find "${SCRIPT_DIR}/${DIST_DIR}" -maxdepth 1 -name "GatOS-*.iso" 2>/dev/null))
  
  if [[ ${#iso_files[@]} -eq 0 ]]; then
    echo -e "${RED}[ERROR] No GatOS ISO file found in ${DIST_DIR}${NC}" >&2
    return 1
  fi
  
  if [[ ${#iso_files[@]} -gt 1 ]]; then
    echo -e "${YELLOW}[WARNING] Multiple ISO files found. Using the most recent one.${NC}" >&2
    
    # Sort by modification time and get the most recent
    local most_recent_iso=$(ls -t "${SCRIPT_DIR}/${DIST_DIR}"/GatOS-*.iso | head -1)
    echo "${most_recent_iso}"
  else
    echo "${iso_files[0]}"
  fi
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
  local iso_file
  iso_file=$(find_iso_file) || return 1

  echo -e "${GREEN}[SUCCESS] Build complete. Starting QEMU with ${iso_file##*/}...${NC}"
  "${QEMU_EXEC}" -cdrom "${iso_file}" -serial mon:stdio -serial file:debug.log
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