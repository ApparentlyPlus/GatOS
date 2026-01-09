#!/usr/bin/env bash
set -euo pipefail

# -----------------------------------------------
#   Universal macOS qemu-system-x86_64 bundle
#   (Auto-Dependency Resolution Version)
# -----------------------------------------------

# --- CONFIGURATION ---
export OSXCROSS_PATH="$HOME/Desktop/osxcross/target/bin"
export PATH="$OSXCROSS_PATH:$PATH"

# Target Main Formula
MAIN_FORMULA="qemu"

# Output Directories
QEMU_ROOT="$HOME/macos-universal-qemu"
WORK="$QEMU_ROOT/build"

# Check for tools
INSTALL_NAME_TOOL="${OSXCROSS_PATH}/x86_64-apple-darwin23.5-install_name_tool"
OTOOL="${OSXCROSS_PATH}/x86_64-apple-darwin23.5-otool"

if [[ ! -x "$INSTALL_NAME_TOOL" || ! -x "$OTOOL" ]]; then
  echo "FATAL: install_name_tool or otool not found at $OSXCROSS_PATH"
  exit 1
fi

if ! command -v jq &> /dev/null; then
    echo "FATAL: 'jq' is required for dependency resolution. Please install it (e.g., sudo apt install jq)."
    exit 1
fi

mkdir -p "$WORK/arm" "$WORK/intel" "$QEMU_ROOT"
cd "$WORK"

# -----------------------------------------------
# 1) Auto-Resolve Dependencies (Recursive)
# -----------------------------------------------
echo "[+] Resolving dependency tree for $MAIN_FORMULA via Homebrew API..."

# Temporary file to store the list
DEPS_FILE="$WORK/deps_list.txt"
: > "$DEPS_FILE"

# Function to recursively fetch dependencies
resolve_deps() {
    local formula=$1
    # API URL for the formula
    local url="https://formulae.brew.sh/api/formula/${formula}.json"
    
    # Fetch JSON
    local json
    if ! json=$(curl -sSf "$url"); then
        echo "Warning: Could not fetch info for $formula. Skipping." >&2
        return
    fi

    # Extract runtime dependencies (not build dependencies)
    # We map them to a space-separated string
    local direct_deps
    direct_deps=$(echo "$json" | jq -r '.dependencies[]')

    for d in $direct_deps; do
        # specific exclusions (system libs or things we don't want)
        if [[ "$d" == "ca-certificates" ]]; then continue; fi

        # check if already seen
        if ! grep -qFx "$d" "$DEPS_FILE"; then
            echo "$d" >> "$DEPS_FILE"
            echo "    Found dependency: $d"
            resolve_deps "$d"
        fi
    done
}

# Start resolution
resolve_deps "$MAIN_FORMULA"

# Read unique deps into array
mapfile -t DEPS < "$DEPS_FILE"
echo "[+] Total dependencies found: ${#DEPS[@]}"

# -----------------------------------------------
# 2) Fetch Bottles
# -----------------------------------------------

TAGS=("arm64_sonoma" "sonoma")

fetch_bottle() {
  local formula=$1
  local tag=$2
  # We use || true because some deps might not have bottles for both archs, 
  # or might be aliases. We try our best.
  brew fetch --force --bottle-tag="$tag" "$formula" >/dev/null 2>&1 || \
    echo "    Warning: Could not fetch bottle for $formula ($tag)"
}

echo "[+] Fetching bottles..."
for F in "$MAIN_FORMULA" "${DEPS[@]}"; do
  # Progress indicator
  echo -n "."
  for T in "${TAGS[@]}"; do
    fetch_bottle "$F" "$T"
  done
done
echo "" # newline

# -----------------------------------------------
# 3) Extract Bottles
# -----------------------------------------------

extract_bottle() {
  local formula=$1
  local tag_arm="arm64_sonoma"
  local tag_intel="sonoma"

  local arm_tar
  local intel_tar

  arm_tar=$(brew --cache --bottle-tag="$tag_arm" "$formula" 2>/dev/null || true)
  intel_tar=$(brew --cache --bottle-tag="$tag_intel" "$formula" 2>/dev/null || true)

  if [[ -f "$arm_tar" ]]; then
      tar -xf "$arm_tar" -C "$WORK/arm" 2>/dev/null || true
  fi
  if [[ -f "$intel_tar" ]]; then
      tar -xf "$intel_tar" -C "$WORK/intel" 2>/dev/null || true
  fi
}

echo "[+] Extracting bottles..."
for F in "$MAIN_FORMULA" "${DEPS[@]}"; do
  extract_bottle "$F"
done

# -----------------------------------------------
# 4) Helpers: safe_find + lipo merge
# -----------------------------------------------

safe_find() {
  local base=$1
  local prefix=$2
  # Some formulas map to different directory names (e.g. openssl@3 -> openssl@3)
  # But sometimes version numbers interfere. We try exact match first.
  local path
  path=$(find "$base" -maxdepth 2 -mindepth 2 -type d -name "$prefix" 2>/dev/null | head -n1 || true)
  
  # If not found, try wildcard
  if [[ -z "$path" ]]; then
      path=$(find "$base" -maxdepth 2 -mindepth 2 -type d -path "$base/$prefix*" 2>/dev/null | head -n1 || true)
  fi
  echo "$path"
}

lipo_merge_tree() {
  local ARM_ROOT="$1"
  local INTEL_ROOT="$2"
  local OUT_ROOT="$3"

  if [[ -d "$ARM_ROOT" && -d "$INTEL_ROOT" ]]; then
      # Base rsync from ARM
      rsync -a "$ARM_ROOT/" "$OUT_ROOT/"
      
      # Merge binaries
      while IFS= read -r f; do
        local rel="${f#$ARM_ROOT/}"
        local a="$ARM_ROOT/$rel"
        local i="$INTEL_ROOT/$rel"
        local o="$OUT_ROOT/$rel"

        if [[ -f "$i" ]] && file "$a" | grep -q "Mach-O"; then
          mkdir -p "$(dirname "$o")"
          lipo -create "$a" "$i" -output "$o" 2>/dev/null || cp "$a" "$o"
        fi
      done < <(find "$ARM_ROOT" -type f)
  elif [[ -d "$ARM_ROOT" ]]; then
      rsync -a "$ARM_ROOT/" "$OUT_ROOT/"
  elif [[ -d "$INTEL_ROOT" ]]; then
      rsync -a "$INTEL_ROOT/" "$OUT_ROOT/"
  fi
}

# -----------------------------------------------
# 5) Assemble and Merge
# -----------------------------------------------

# QEMU (Main)
QEMU_ARM_ROOT=$(safe_find "$WORK/arm" "$MAIN_FORMULA")
QEMU_INTEL_ROOT=$(safe_find "$WORK/intel" "$MAIN_FORMULA")
QEMU_UNI="$WORK/universal-qemu"

mkdir -p "$QEMU_UNI"
lipo_merge_tree "$QEMU_ARM_ROOT" "$QEMU_INTEL_ROOT" "$QEMU_UNI"

echo "[+] Assembling bundle in $QEMU_ROOT..."
mkdir -p "$QEMU_ROOT/bin" "$QEMU_ROOT/lib" "$QEMU_ROOT/share"

# Copy QEMU Main Binaries
if [[ -d "$QEMU_UNI/bin" ]]; then
    cp "$QEMU_UNI/bin/qemu-system-x86_64" "$QEMU_ROOT/bin/"
else
    echo "FATAL: Build failed, qemu binary not found."
    exit 1
fi

# Copy QEMU Share
if [[ -d "$QEMU_UNI/share/qemu" ]]; then
    rsync -a "$QEMU_UNI/share/qemu" "$QEMU_ROOT/share/"
fi

# Process Deps
for F in "${DEPS[@]}"; do
  ARM_ROOT=$(safe_find "$WORK/arm" "$F")
  INTEL_ROOT=$(safe_find "$WORK/intel" "$F")
  
  if [[ -z "$ARM_ROOT" && -z "$INTEL_ROOT" ]]; then
      continue
  fi

  UNI_ROOT="$WORK/universal-$F"
  mkdir -p "$UNI_ROOT"
  lipo_merge_tree "$ARM_ROOT" "$INTEL_ROOT" "$UNI_ROOT"

  if [[ -d "$UNI_ROOT/lib" ]]; then
    rsync -a "$UNI_ROOT/lib/" "$QEMU_ROOT/lib/"
  fi
done

# -----------------------------------------------
# 6) Rewrite dylib paths (The Magic Fix)
# -----------------------------------------------

export LIB_DIR="$QEMU_ROOT/lib"

patch_binary() {
  local file="$1"
  # Add rpath
  "$INSTALL_NAME_TOOL" -add_rpath "@loader_path/../lib" "$file" 2>/dev/null || true

  # Read dynamic links
  "$OTOOL" -L "$file" | tail -n +2 | while read -r dep _; do
    dep=$(echo "$dep" | xargs) # trim whitespace
    
    # Skip system libs
    if [[ "$dep" == /usr/lib/* || "$dep" == /System/Library/* ]]; then continue; fi
    
    local base=$(basename "$dep")
    local target="@rpath/$base"
    
    # Handle versioned libs logic if simple rewrite fails? 
    # For now, simple rewrite works for 99% of homebrew bottles.
    
    if [[ "$dep" != "@rpath"* ]]; then
       "$INSTALL_NAME_TOOL" -change "$dep" "$target" "$file" 2>/dev/null || true
    fi
  done

  # Fix ID if it is a dylib
  if [[ "$file" == *.dylib ]]; then
    "$INSTALL_NAME_TOOL" -id "@rpath/$(basename "$file")" "$file" 2>/dev/null || true
  fi
}

echo "[+] Patching binaries for portability..."
find "$QEMU_ROOT" -type f | while read -r f; do
    if file "$f" | grep -q "Mach-O"; then
        patch_binary "$f"
    fi
done

echo ""
echo "--------------------------------------------------"
echo "SUCCESS: Bundle created at $QEMU_ROOT"
echo "--------------------------------------------------"
echo "Copy '$QEMU_ROOT' to your Mac and run:"
echo "  ./bin/qemu-system-x86_64 --version"