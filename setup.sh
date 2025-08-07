#!/bin/bash
set -euo pipefail

# Colors for kewl text output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[94m'
YELLOW='\033[1;33m'
NC='\033[0m'
BPURPLE='\033[1;35m'

echo -e "${BLUE}GatOS Build Environment Setup${NC}"
echo

# Not cross platform yet
OS="$(uname)"
if [[ "$OS" != "Linux" ]]; then
  echo -e "${RED}ERROR:${NC} GatOS currently supports Linux only."
  echo "Please use a Linux environment to build GatOS."
  echo "MacOS build support will *most likely* be supported soon."
  echo "Until then, use the prebuild images to run everything in QEMU (assuming you install it in your system)"
  exit 1
fi

# Are we root? If not, use sudo (we should be tho)
if [[ "$(id -u)" -ne 0 ]]; then
  SUDO="sudo"
else
  SUDO=""
fi

# Improved command check that also verifies the command is executable
function ensure_command() {
  local cmd="$1"
  local pkg="$2"
  
  if command -v "$cmd" &> /dev/null && [ -x "$(command -v "$cmd")" ]; then
    echo -e "${GREEN}[✓] '$cmd' is installed and executable${NC}"
    return 0
  fi
  
  echo -e "${YELLOW}[!] '$cmd' not found or not executable. Installing package '$pkg'...${NC}"
  $SUDO apt update -y
  if ! $SUDO apt install -y "$pkg"; then
    echo -e "${RED}[X] Failed to install '$pkg'${NC}"
    return 1
  fi
}

# Ensure bash exists (Homebrew install script requires bash)
ensure_command bash bash || {
  echo -e "${RED}[X] bash is required but not installed${NC}"
  exit 1
}

# Ensure curl is installed before we try to use it
ensure_command curl curl || {
  echo -e "${RED}[X] curl is required but not installed${NC}"
  exit 1
}

# Install Homebrew dependencies first
echo -e "${BLUE}[*] Installing Homebrew dependencies...${NC}"
HOMEBREW_DEPS=(build-essential procps file git)
for pkg in "${HOMEBREW_DEPS[@]}"; do
  if ! dpkg -s "$pkg" &> /dev/null; then
    echo -e "${BLUE}[*] Installing package: $pkg${NC}"
    $SUDO apt install -y "$pkg" || {
      echo -e "${YELLOW}[!] Failed to install $pkg${NC}"
    }
  else
    echo -e "${GREEN}[✓] Package $pkg already installed${NC}"
  fi
done

# Install Homebrew if missing
BREW_PREFIX="/home/linuxbrew/.linuxbrew"
if ! command -v brew &> /dev/null; then
  echo -e "${BLUE}[*] Homebrew not found. Installing Homebrew...${NC}"
  NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)" || {
    echo -e "${RED}[X] Homebrew installation failed${NC}"
    exit 1
  }

  # Verify brew binary exists
  if [[ ! -f "${BREW_PREFIX}/bin/brew" ]]; then
    echo -e "${RED}[X] Homebrew install failed - brew binary missing${NC}"
    exit 1
  fi

  # Add Homebrew to PATH immediately for current session
  eval "$(${BREW_PREFIX}/bin/brew shellenv)"
  echo -e "${GREEN}[✓] Homebrew installed successfully${NC}"
else
  echo -e "${GREEN}[✓] Homebrew already installed${NC}"
  BREW_PREFIX="$(brew --prefix)"
fi

# Install x86_64-elf-gcc if not installed
if ! brew list x86_64-elf-gcc &> /dev/null; then
  echo -e "${BLUE}[*] Installing x86_64-elf-gcc via Homebrew...${NC}"
  brew install -q x86_64-elf-gcc || {
    echo -e "${RED}[X] Failed to install x86_64-elf-gcc${NC}"
    exit 1
  }
else
  echo -e "${GREEN}[✓] x86_64-elf-gcc already installed via Homebrew${NC}"
fi

# Update PATH to include Homebrew's x86_64-elf-gcc
export PATH="${BREW_PREFIX}/bin:${PATH}"

# Detect shell and add brew to rc file
RC_FILE="${HOME}/.bashrc"
if [[ -n "${ZSH_VERSION-}" ]]; then
  RC_FILE="${HOME}/.zshrc"
elif [[ -f "${HOME}/.profile" && ! -f "${HOME}/.bashrc" ]]; then
  RC_FILE="${HOME}/.profile"
fi

# Add Homebrew env if missing
if ! grep -q 'brew shellenv' "$RC_FILE" 2>/dev/null; then
  echo -e "${BLUE}[*] Adding Homebrew environment to $RC_FILE${NC}"
  {
    echo ''
    echo '# Set PATH for Homebrew'
    echo 'eval "$(/home/linuxbrew/.linuxbrew/bin/brew shellenv)"'
  } >> "$RC_FILE"
  echo -e "${GREEN}[✓] Added Homebrew to $RC_FILE${NC}"
else
  echo -e "${GREEN}[✓] Homebrew already in $RC_FILE${NC}"
fi

# Update package lists
echo -e "${BLUE}[*] Updating system package lists...${NC}"
$SUDO apt update -y || {
  echo -e "${YELLOW}[!] Package list update failed, continuing anyway${NC}"
}

# Install GatOS build dependencies
REQUIRED_PKGS=(build-essential nasm xorriso grub-pc-bin qemu-system-x86)
echo -e "${BLUE}[*] Installing required packages: ${REQUIRED_PKGS[*]}${NC}"
for pkg in "${REQUIRED_PKGS[@]}"; do
  if ! dpkg -s "$pkg" &> /dev/null; then
    echo -e "${BLUE}[*] Installing package: $pkg${NC}"
    $SUDO apt install -y "$pkg" || {
      echo -e "${YELLOW}[!] Failed to install $pkg${NC}"
    }
  else
    echo -e "${GREEN}[✓] Package $pkg already installed${NC}"
  fi
done

echo
echo -e "${GREEN}[+] Setup Complete!${NC}"
echo
echo -e "${BPURPLE}You MUST restart your shell or run:${BPURPLE}"
echo -e "    ${BLUE}source $RC_FILE${NC}"
echo "to ensure all environment variables are properly set."
echo
echo "You can now build GatOS by running:"
echo -e "    ${BLUE}make${NC}"
echo
echo "To build and run GatOS in QEMU, use:"
echo -e "    ${BLUE}chmod +x run.sh${NC}"
echo -e "    ${BLUE}./run.sh${NC}"
echo

# Append PATH to GitHub Actions environment if running there
if [[ -n "${GITHUB_ENV:-}" ]]; then
  echo "export PATH=${BREW_PREFIX}/bin:\$PATH" >> "$GITHUB_ENV"
fi
