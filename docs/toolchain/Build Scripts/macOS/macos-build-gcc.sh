#!/bin/bash

# Run with bash get-gcc.sh gcc binutils macos -gv 13.2.0 -bv 2.41 -64 -parallel zip

# Define Global Variables

BINUTILS_VERSION=2.28
GCC_VERSION=13.2.0
GDB_VERSION=8.0

BUILD_TARGET="i686-elf"

set -e

ALL_PRODUCTS=true

# Parse Commandline Options

if [ $# -eq 0 ]; then
    BUILD_BINUTILS=true
    BUILD_GCC=true
    BUILD_GDB=true
    ZIP=true
    PARALLEL=true
    args="binutils gcc gdb zip"
else
    args=$@
fi

while [[ $# -gt 0 ]]
do
key="$1"

case $key in
    binutils)               BUILD_BINUTILS=true;   ALL_PRODUCTS=false; shift ;;
    gcc)                    BUILD_GCC=true;        ALL_PRODUCTS=false; shift ;;
    gdb)                    BUILD_GDB=true;        ALL_PRODUCTS=false; shift ;;
    win)                    WINDOWS_ONLY=true;                         shift ;;
    linux)                  LINUX_ONLY=true;                           shift ;;
    macos)                  MACOS_ONLY=true;                           shift ;;
    zip)                    ZIP=true;              ALL_PRODUCTS=false; shift ;;
    env)                    ENV_ONLY=true;                             shift ;;
    -64)                    x64=true;                                  shift ;;
    -parallel)              PARALLEL=true;                             shift ;;
    -bv|--binutils-version) BINUTILS_VERSION="$2";                     shift; shift ;;
    -gv|--gcc-version)      GCC_VERSION="$2";                          shift; shift ;;
    -dv|--gdb-version)      GDB_VERSION="$2";                          shift; shift ;;
    *)                                                                 shift ;;
esac
done

if [[ $x64 == true ]]; then
    BUILD_TARGET="x86_64-elf"
fi

BUILD_DIR="$HOME/build-${BUILD_TARGET}"
export OSXCROSS_PATH="$HOME/Desktop/osxcross/target/bin"
export PATH="/opt/mxe/usr/bin:$BUILD_DIR/linux/output/bin:$BUILD_DIR/windows/output/bin:$OSXCROSS_PATH:$PATH"

echo "BUILD_TARGET     = ${BUILD_TARGET}"
echo "BUILD_DIR        = ${BUILD_DIR}"
echo "BUILD_BINUTILS   = ${BUILD_BINUTILS}"
echo "BUILD_GCC        = ${BUILD_GCC}"
echo "BUILD_GDB        = ${BUILD_GDB}"
echo "ZIP              = ${ZIP}"
echo "WIN              = ${WINDOWS_ONLY}"
echo "LINUX            = ${LINUX_ONLY}"
echo "MACOS            = ${MACOS_ONLY}"
echo "ENV              = ${ENV_ONLY}"
echo "x64              = ${x64}"
echo "BINUTILS_VERSION = ${BINUTILS_VERSION}"
echo "GCC_VERSION      = ${GCC_VERSION}"
echo "GDB_VERSION      = ${GDB_VERSION}"
echo "PATH             = ${PATH}"
echo "PARALLEL         = ${PARALLEL}"

function main {

    installPackages
    
    if [[ $MACOS_ONLY != true ]]; then
        installMXE
    else
        echoColor "Skipping MXE installation as 'macos' was specified in commandline args '$args'"
    fi
    
    if [[ $ENV_ONLY == true ]]; then
        echoColor "Successfully installed build environment. Exiting as 'env' only was specified"
        return
    fi
    
    downloadSources
    
    if [[ $WINDOWS_ONLY == true || $MACOS_ONLY == true ]]; then
        echoColor "Skipping compiling Linux as 'win' or 'macos' was specified in commandline args '$args'"
    else
        compileAll "linux"
    fi
    
    if [[ $LINUX_ONLY == true || $MACOS_ONLY == true ]]; then
        echoColor "Skipping compiling Windows as 'linux' or 'macos' was specified in commandline args '$args'"
    else
        compileAll "windows"
    fi
    
    if [[ $LINUX_ONLY == true || $WINDOWS_ONLY == true ]]; then
        echoColor "Skipping compiling macOS as 'linux' or 'win' was specified in commandline args '$args'"
    else
        compileAllMacOS
    fi
        
    finalize
}

function installPackages {
    pkgList=(
        git
        autoconf
        automake
        autopoint
        bash
        bison
        bzip2
        flex
        gettext
        g++
        gperf
        intltool
        libffi-dev
        libgdk-pixbuf-2.0-dev
        libgmp-dev
        libmpfr-dev
        libtool
        libltdl-dev
        libssl-dev
        libxml-parser-perl
        make
        python3-mako
        openssl
        p7zip-full
        patch
        perl
        pkg-config
        ruby
        scons
        sed
        unzip
        wget
        xz-utils
        libtool-bin
        texinfo
        g++-multilib
        lzip)
    echoColor "Installing packages"

    # Fix correct python packages on modern Ubuntu and Ubuntu-based distros
    if [[ $(apt-cache search --names-only ^python3$ | wc -m) -gt 0 ]]; then
        pkgList+=(python3 python-is-python3)
    else
        pkgList+=(python)
    fi

    for pkg in ${pkgList[@]}; do
        sudo -E DEBIAN_FRONTEND=noninteractive apt-get -qq install $pkg -y
    done
}

# MXE
function installMXE {

    echoColor "Installing MXE"

    if [ ! -d "/opt/mxe/usr/bin" ]
    then
        echoColor "    Cloning MXE and compiling mingw32.static GCC"
        cd /opt
        sudo -E git clone https://github.com/mxe/mxe.git
        cd mxe
        if [[ $PARALLEL == true ]]; then
            sudo make -j6 gcc gmp mpfr
        else
            sudo make gcc gmp mpfr
        fi
    else
       echoColor "    MXE is already installed. You'd better make sure that you've previously made MXE's gcc! (/opt/mxe/usr/bin/i686-w64-mingw32.static-gcc)"
    fi
}

# Downloads

function downloadSources {
    mkdir -p $BUILD_DIR
    cd $BUILD_DIR
    
    echoColor "Downloading all sources"
    
    if [[ $BUILD_BINUTILS == true || $ALL_PRODUCTS == true ]]; then
        downloadAndExtract "binutils" $BINUTILS_VERSION
    else
        echoColor "    Skipping binutils as 'binutils' was ommitted from commandline args '$args'"
    fi
    
    if [[ $BUILD_GCC == true || $ALL_PRODUCTS == true ]]; then
        downloadAndExtract "gcc" $GCC_VERSION "http://ftpmirror.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.gz"
        
        echoColor "        Downloading GCC prerequisites"
        
        # Automatically download GMP, MPC and MPFR. These will be placed into the right directories.
        # You can also download these separately, and specify their locations as arguments to ./configure
        
        if [[ $WINDOWS_ONLY != true && $MACOS_ONLY != true ]]; then
            echoColor "            Linux"
            cd ./linux/gcc-$GCC_VERSION
            ./contrib/download_prerequisites
        fi
        
        cd $BUILD_DIR
        
        if [[ $LINUX_ONLY != true && $MACOS_ONLY != true ]]; then
            echoColor "            Windows"
            cd ./windows/gcc-$GCC_VERSION
            ./contrib/download_prerequisites
        fi
        
        cd $BUILD_DIR
        
        if [[ $LINUX_ONLY != true && $WINDOWS_ONLY != true ]]; then
            echoColor "            macOS (arm64)"
            cd ./macos-arm64/gcc-$GCC_VERSION
            ./contrib/download_prerequisites
            
            cd $BUILD_DIR
            
            echoColor "            macOS (x86_64)"
            cd ./macos-x86_64/gcc-$GCC_VERSION
            ./contrib/download_prerequisites
        fi
        
        cd $BUILD_DIR
    else
        echoColor "    Skipping gcc as 'gcc' was ommitted from commandline args '$args'"
    fi
    
    if [[ $BUILD_GDB == true || $ALL_PRODUCTS == true ]]; then
        downloadAndExtract "gdb" $GDB_VERSION
    else
       echoColor "    Skipping gdb as 'gdb' was ommitted from commandline args '$args'"
    fi
}

function downloadAndExtract {
    name=$1
    version=$2
    override=$3
    
    echoColor "    Processing $name"
    
    if [ ! -f $name-$version.tar.gz ]
    then
        echoColor "        Downloading $name-$version.tar.gz"
        
        if [ -z $3 ]
        then
            wget -q http://ftpmirror.gnu.org/gnu/$name/$name-$version.tar.gz
        else
            wget -q $override
        fi
    else
        echoColor "        $name-$version.tar.gz already exists"
    fi

    if [[ $WINDOWS_ONLY == true || $MACOS_ONLY == true ]]; then
        echoColor "        Skipping extracting Linux as 'win' or 'macos' was specified in commandline args '$args'"
    else
        mkdir -p linux
        cd linux
        
        if [ ! -d $name-$version ]
        then
            echoColor "        [linux]   Extracting $name-$version.tar.gz"
            tar -xf ../$name-$version.tar.gz
        else
            echoColor "        [linux]   Folder $name-$version already exists"
        fi
        
        cd ..
    fi
    
    if [[ $LINUX_ONLY == true || $MACOS_ONLY == true ]]; then
        echoColor "        Skipping extracting Windows as 'linux' or 'macos' was specified in commandline args '$args'"
    else
        mkdir -p windows
        cd windows
        
        if [ ! -d $name-$version ]
        then
            echoColor "        [windows] Extracting $name-$version.tar.gz"
            tar -xf ../$name-$version.tar.gz
        else
            echoColor "        [windows] Folder $name-$version already exists"
        fi
        
        cd ..
    fi
    
    if [[ $LINUX_ONLY == true || $WINDOWS_ONLY == true ]]; then
        echoColor "        Skipping extracting macOS as 'linux' or 'win' was specified in commandline args '$args'"
    else
        mkdir -p macos-arm64
        cd macos-arm64
        
        if [ ! -d $name-$version ]
        then
            echoColor "        [macos-arm64] Extracting $name-$version.tar.gz"
            tar -xf ../$name-$version.tar.gz
        else
            echoColor "        [macos-arm64] Folder $name-$version already exists"
        fi
        
        cd ..
        
        mkdir -p macos-x86_64
        cd macos-x86_64
        
        if [ ! -d $name-$version ]
        then
            echoColor "        [macos-x86_64] Extracting $name-$version.tar.gz"
            tar -xf ../$name-$version.tar.gz
        else
            echoColor "        [macos-x86_64] Folder $name-$version already exists"
        fi
        
        cd ..
    fi
}

function compileAll {

    echoColor "Compiling all $1"
    
    cd $1
    
    mkdir -p output

    compileBinutils $1
    compileGCC $1
    compileGDB $1
    
    cd ..
}

function compileAllMacOS {

    echoColor "Compiling all macOS (universal binaries)"
    
    # Build for ARM64
    cd macos-arm64
    mkdir -p output
    compileBinutils "macos-arm64"
    compileGCC "macos-arm64"
    compileGDB "macos-arm64"
    cd ..
    
    # Build for x86_64
    cd macos-x86_64
    mkdir -p output
    compileBinutils "macos-x86_64"
    compileGCC "macos-x86_64"
    compileGDB "macos-x86_64"
    cd ..
    
    # Create universal binaries
    createUniversalBinaries
}

function createUniversalBinaries {
    echoColor "Creating universal binaries for macOS"
    
    # We must use ABSOLUTE PATHS to avoid directory confusion when 'cd'ing.
    # $BUILD_DIR is already absolute (see top of script: BUILD_DIR="$HOME/build-${BUILD_TARGET}")
    
    ARM_SRC="$BUILD_DIR/macos-arm64/output"
    X86_SRC="$BUILD_DIR/macos-x86_64/output"
    MAC_OUT="$BUILD_DIR/macos/output"
    
    mkdir -p "$MAC_OUT"
    
    echoColor "    1. Cleaning previous output..."
    rm -rf "$MAC_OUT"
    mkdir -p "$MAC_OUT"
    
    echoColor "    2. Copying ARM64 build as base..."
    cp -R "$ARM_SRC/"* "$MAC_OUT/"
    
    echoColor "    3. Scanning and merging Mach-O executables..."
    
    # Move into the output directory to run finding loop
    cd "$MAC_OUT"
    
    # Find all files recursively
    find . -type f | while read FILE; do
        # Strip leading ./ for clean matching
        CLEAN_FILE="${FILE#./}"
        
        if [[ "$CLEAN_FILE" == *"lib/gcc/x86_64-elf"* ]]; then
            continue
        fi
        
        # Define x86 counterpart absolute path
        X86_FILE="$X86_SRC/$CLEAN_FILE"
        
        # Check file type using 'file' command
        FILE_TYPE=$(file -b "$FILE")
        
        # We look for "Mach-O" (binaries/dylibs) or "ar archive" (host static libs)
        if [[ "$FILE_TYPE" == *"Mach-O"* ]] || [[ "$FILE_TYPE" == *"ar archive"* ]]; then
            if [ -f "$X86_FILE" ]; then
                # echo "        Merging: $CLEAN_FILE"
                lipo -create "$FILE" "$X86_FILE" -output "$FILE"
            else
                echoColor "        [WARN] No x86_64 counterpart found for: $CLEAN_FILE"
            fi
        fi
    done
    
    cd ../..
    
    echoColor "    Universal binaries created successfully."
}

function compileBinutils {
    if [[ $BUILD_BINUTILS == true || $ALL_PRODUCTS == true ]]; then
        echoColor "    Compiling binutils [$1]"
    
        mkdir -p build-binutils-$BINUTILS_VERSION
        cd build-binutils-$BINUTILS_VERSION
        
        configureArgs="--target=${BUILD_TARGET} --with-sysroot --disable-nls --disable-werror --disable-plugins --prefix=$BUILD_DIR/$1/output"
        
        # Set up environment for cross-compilation
        unset AR RANLIB CC CXX LD AS NM STRIP OBJDUMP CFLAGS CXXFLAGS
        
        if [ $1 == "windows" ]
        then
            configureArgs="--host=i686-w64-mingw32.static $configureArgs"
        elif [ $1 == "macos-arm64" ]
        then
            configureArgs="--host=aarch64-apple-darwin23.5 $configureArgs"
            export CC=arm64-apple-darwin23.5-clang
            export CXX=arm64-apple-darwin23.5-clang++
            export AR=arm64-apple-darwin23.5-ar
            export RANLIB=arm64-apple-darwin23.5-ranlib
            export AS=arm64-apple-darwin23.5-as
            export LD=arm64-apple-darwin23.5-ld
            export CFLAGS="-Dfdopen=fdopen"
            export CXXFLAGS="-Dfdopen=fdopen"
        elif [ $1 == "macos-x86_64" ]
        then
            configureArgs="--host=x86_64-apple-darwin23.5 $configureArgs"
            export CC=x86_64-apple-darwin23.5-clang
            export CXX=x86_64-apple-darwin23.5-clang++
            export AR=x86_64-apple-darwin23.5-ar
            export RANLIB=x86_64-apple-darwin23.5-ranlib
            export AS=x86_64-apple-darwin23.5-as
            export LD=x86_64-apple-darwin23.5-ld
            export CFLAGS="-Dfdopen=fdopen"
            export CXXFLAGS="-Dfdopen=fdopen"
        fi
        
        # Configure
        echoColor "        Configuring binutils (binutils_configure.log)"
        ../binutils-$BINUTILS_VERSION/configure $configureArgs >> binutils_configure.log
        
        # Make
        echoColor "        Making (binutils_make.log)"
        if [[ $PARALLEL == true ]]; then
            make -j6 >> binutils_make.log
        else
            make >> binutils_make.log
        fi
        
        # Install
        echoColor "        Installing (binutils_install.log)"
        make install >> binutils_install.log
        
        # Clean up environment variables
        unset AR RANLIB CC CXX LD AS NM STRIP OBJDUMP CFLAGS CXXFLAGS
        
        cd ..
    else
        echoColor "    Skipping binutils [$1] as 'binutils' was ommitted from commandline args '$args'"
    fi
}

function compileGCC {
    if [[ $BUILD_GCC == true || $ALL_PRODUCTS == true ]]; then
    
        echoColor "    Compiling gcc [$1]"

        mkdir -p build-gcc-$GCC_VERSION
        cd build-gcc-$GCC_VERSION
        
        # --- FIX: Patch GCC 13 source to prevent libc++ / safe-ctype.h conflicts ---
        echoColor "        Patching GCC C++ files to fix macOS libc++ conflicts..."
        
        # Fix: GCC 15+ headers from Linuxbrew
        if [ -f ../gcc-$GCC_VERSION/gcc/common/config/i386/cpuinfo.h ]; then
            echoColor "        Patching cpuinfo.h to disable obsolete Xeon Phi checks..."
            sed -i 's/bit_PREFETCHWT1/0/g' ../gcc-$GCC_VERSION/gcc/common/config/i386/cpuinfo.h
            sed -i 's/bit_AVX512PF/0/g' ../gcc-$GCC_VERSION/gcc/common/config/i386/cpuinfo.h
            sed -i 's/bit_AVX512ER/0/g' ../gcc-$GCC_VERSION/gcc/common/config/i386/cpuinfo.h
            sed -i 's/bit_AVX5124VNNIW/0/g' ../gcc-$GCC_VERSION/gcc/common/config/i386/cpuinfo.h
            sed -i 's/bit_AVX5124FMAPS/0/g' ../gcc-$GCC_VERSION/gcc/common/config/i386/cpuinfo.h
        fi
        
        # GCC 13 needs C++ support in the bootstrap compiler
        configureArgs="--target=${BUILD_TARGET} --disable-nls --enable-languages=c,c++ --without-headers --disable-plugins --prefix=$BUILD_DIR/$1/output"
        
        # Clear env to prevent contamination
        unset AR RANLIB CC CXX LD AS NM STRIP OBJDUMP CFLAGS CXXFLAGS
        
        if [ $1 == "windows" ]
        then
            configureArgs="--host=i686-w64-mingw32.static $configureArgs"
        elif [ $1 == "macos-arm64" ]
        then
            configureArgs="--host=aarch64-apple-darwin23.5 $configureArgs"
            export CC=aarch64-apple-darwin23.5-clang
            export CXX=aarch64-apple-darwin23.5-clang++
            export AR=aarch64-apple-darwin23.5-ar
            export RANLIB=aarch64-apple-darwin23.5-ranlib
            export AS=aarch64-apple-darwin23.5-as
            export LD=aarch64-apple-darwin23.5-ld
            export CFLAGS="-Dfdopen=fdopen -DHAVE_STRSIGNAL"
            export CXXFLAGS="-Dfdopen=fdopen -include cstdlib -include vector -DHAVE_STRSIGNAL"
        elif [ $1 == "macos-x86_64" ]
        then
            configureArgs="--host=x86_64-apple-darwin23.5 $configureArgs"
            export CC=x86_64-apple-darwin23.5-clang
            export CXX=x86_64-apple-darwin23.5-clang++
            export AR=x86_64-apple-darwin23.5-ar
            export RANLIB=x86_64-apple-darwin23.5-ranlib
            export AS=x86_64-apple-darwin23.5-as
            export LD=x86_64-apple-darwin23.5-ld
            export CFLAGS="-Dfdopen=fdopen -DHAVE_STRSIGNAL"
            export CXXFLAGS="-Dfdopen=fdopen -include cstdlib -include vector -DHAVE_STRSIGNAL"
        fi
        
        # If building linux, add self to path to find the compiler we just built.
        # If MacOS/Win, we will use system compiler via override later.
        if [ "$1" == "linux" ]; then
             export PATH="$BUILD_DIR/$1/output/bin:$PATH"
        fi
        
        # x64 no-red-zone patching
        if [[ $x64 == true ]]; then
            echoColor "        Installing config/i386/t-x86_64-elf"
            echo -e "# Add libgcc multilib variant without red-zone requirement\n\nMULTILIB_OPTIONS += mno-red-zone\nMULTILIB_DIRNAMES += no-red-zone" > ../gcc-$GCC_VERSION/gcc/config/i386/t-x86_64-elf
            
            echoColor "        Patching gcc/config.gcc"
            if ! grep -q "t-x86_64-elf" ../gcc-$GCC_VERSION/gcc/config.gcc; then
                sed -i '/x86_64-\*-elf\*)/a \\ttmake_file="${tmake_file} i386/t-x86_64-elf" # include the new multilib configuration' ../gcc-$GCC_VERSION/gcc/config.gcc
            fi
        fi
        
        # Configure
        echoColor "        Configuring gcc (gcc_configure.log)"
        ../gcc-$GCC_VERSION/configure $configureArgs >> gcc_configure.log
        
        # Make GCC
        echoColor "        Making gcc (gcc_make.log)"
        if [[ $PARALLEL == true ]]; then
            make -j6 all-gcc >> gcc_make.log
        else
            make all-gcc >> gcc_make.log
        fi
        
        # Install GCC
        echoColor "        Installing gcc (gcc_install.log)"
        make install-gcc >> gcc_install.log
        
        # --- LIBGCC BUILD LOGIC ---
        
        echoColor "        Making libgcc (libgcc_make.log)"
        
        if [ "$1" == "linux" ]; then
            # Normal build using the compiler we just built and added to PATH
            if [[ $PARALLEL == true ]]; then
                make -j6 all-target-libgcc >> libgcc_make.log
            else
                make all-target-libgcc >> libgcc_make.log
            fi
            
            echoColor "        Installing libgcc (libgcc_install.log)"
            make install-target-libgcc >> libgcc_install.log

        else
            # Canadian Cross (MacOS/Windows on Linux)
            # Use SYSTEM compiler since we are on Linux and have it installed
            echoColor "        [Canadian Cross] Forcing build using system x86_64-elf-gcc..."
            
            if ! command -v x86_64-elf-gcc &> /dev/null; then
                 echoError "        CRITICAL: x86_64-elf-gcc not found in PATH! install it or add to PATH."
                 exit 1
            fi

            # Override Make variables to force system compiler
            if [[ $PARALLEL == true ]]; then
                make -j6 all-target-libgcc \
                    CC_FOR_TARGET="x86_64-elf-gcc" \
                    GCC_FOR_TARGET="x86_64-elf-gcc" \
                    AR_FOR_TARGET="x86_64-elf-ar" \
                    AS_FOR_TARGET="x86_64-elf-as" \
                    LD_FOR_TARGET="x86_64-elf-ld" \
                    NM_FOR_TARGET="x86_64-elf-nm" \
                    RANLIB_FOR_TARGET="x86_64-elf-ranlib" >> libgcc_make.log
            else
                make all-target-libgcc \
                    CC_FOR_TARGET="x86_64-elf-gcc" \
                    GCC_FOR_TARGET="x86_64-elf-gcc" \
                    AR_FOR_TARGET="x86_64-elf-ar" \
                    AS_FOR_TARGET="x86_64-elf-as" \
                    LD_FOR_TARGET="x86_64-elf-ld" \
                    NM_FOR_TARGET="x86_64-elf-nm" \
                    RANLIB_FOR_TARGET="x86_64-elf-ranlib" >> libgcc_make.log
            fi

            echoColor "        Installing libgcc (libgcc_install.log)"
            make install-target-libgcc \
                CC_FOR_TARGET="x86_64-elf-gcc" \
                GCC_FOR_TARGET="x86_64-elf-gcc" \
                AR_FOR_TARGET="x86_64-elf-ar" \
                AS_FOR_TARGET="x86_64-elf-as" \
                LD_FOR_TARGET="x86_64-elf-ld" \
                NM_FOR_TARGET="x86_64-elf-nm" \
                RANLIB_FOR_TARGET="x86_64-elf-ranlib" >> libgcc_install.log
        fi
        
        # Install no-red-zone if needed
        if [[ $x64 == true ]]; then
            if [ $1 == "windows" ] || [ $1 == "macos-arm64" ] || [ $1 == "macos-x86_64" ]; then
                 if [ -d "${BUILD_TARGET}/no-red-zone/libgcc" ]; then
                    cd "${BUILD_TARGET}/no-red-zone/libgcc"
                    make install \
                        CC_FOR_TARGET="x86_64-elf-gcc" \
                        GCC_FOR_TARGET="x86_64-elf-gcc" \
                        AR_FOR_TARGET="x86_64-elf-ar" \
                        AS_FOR_TARGET="x86_64-elf-as" \
                        LD_FOR_TARGET="x86_64-elf-ld" \
                        NM_FOR_TARGET="x86_64-elf-nm" \
                        RANLIB_FOR_TARGET="x86_64-elf-ranlib" >> ../../../libgcc_install_noredzone.log
                    cd ../../..
                 fi
            fi
        fi
        
        unset AR RANLIB CC CXX LD AS NM STRIP OBJDUMP CFLAGS CXXFLAGS
        
        cd ..
    else
        echoColor "    Skipping gcc [$1] as 'gcc' was ommitted from commandline args '$args'"
    fi
}

function compileGDB {
    if [[ $BUILD_GDB == true || $ALL_PRODUCTS == true ]]; then

        echoColor "    Compiling gdb [$1]"
    
        configureArgs="--target=${BUILD_TARGET} --disable-nls --disable-werror --disable-plugins --prefix=$BUILD_DIR/$1/output"
        
        # Set up environment for cross-compilation
        unset AR RANLIB CC CXX LD AS NM STRIP OBJDUMP CFLAGS CXXFLAGS
        
        if [ $1 == "windows" ]
        then
            configureArgs="--host=i686-w64-mingw32.static $configureArgs"
        elif [ $1 == "macos-arm64" ]
        then
            configureArgs="--host=arm64-apple-darwin23.5 $configureArgs"
            export CC=arm64-apple-darwin23.5-clang
            export CXX=arm64-apple-darwin23.5-clang++
            export AR=arm64-apple-darwin23.5-ar
            export RANLIB=arm64-apple-darwin23.5-ranlib
            export AS=arm64-apple-darwin23.5-as
            export LD=arm64-apple-darwin23.5-ld
            export CFLAGS="-Dfdopen=fdopen"
            export CXXFLAGS="-Dfdopen=fdopen"
        elif [ $1 == "macos-x86_64" ]
        then
            configureArgs="--host=x86_64-apple-darwin23.5 $configureArgs"
            export CC=x86_64-apple-darwin23.5-clang
            export CXX=x86_64-apple-darwin23.5-clang++
            export AR=x86_64-apple-darwin23.5-ar
            export RANLIB=x86_64-apple-darwin23.5-ranlib
            export AS=x86_64-apple-darwin23.5-as
            export LD=x86_64-apple-darwin23.5-ld
            export CFLAGS="-Dfdopen=fdopen"
            export CXXFLAGS="-Dfdopen=fdopen"
        fi
    
        mkdir -p build-gdb-$GDB_VERSION
        cd build-gdb-$GDB_VERSION
        
        # Configure
        echoColor "        Configuring (gdb_configure.log)"
        ../gdb-$GDB_VERSION/configure $configureArgs >> gdb_configure.log
        
        # Make
        echoColor "        Making (gdb_make.log)"
        if [[ $PARALLEL == true ]]; then
            make -j6 >> gdb_make.log
        else
            make >> gdb_make.log
        fi
        
        # Install
        echoColor "        Installing (gdb_install.log)"
        make install >> gdb_install.log
        
        # Clean up environment variables
        unset AR RANLIB CC CXX LD AS NM STRIP OBJDUMP CFLAGS CXXFLAGS
        
        cd ..
    else
        echoColor "    Skipping gdb [$1] as 'gdb' was ommitted from commandline args '$args'"
    fi
}

function finalize {
    if [[ $ZIP == true || $ALL_PRODUCTS == true ]]; then
        echo "Zipping everything up!"
        
        if [[ -d "$BUILD_DIR/windows/output" ]]; then
            cd $BUILD_DIR/windows/output
            zip -r "${BUILD_DIR}/${BUILD_TARGET}-tools-windows.zip" *
        fi
        
        if [[ -d "$BUILD_DIR/linux/output" ]]; then
            cd $BUILD_DIR/linux/output
            zip -r "${BUILD_DIR}/${BUILD_TARGET}-tools-linux.zip" *
        fi
        
        if [[ -d "$BUILD_DIR/macos/output" ]]; then
            cd $BUILD_DIR/macos/output
            zip -r "${BUILD_DIR}/${BUILD_TARGET}-tools-macos.zip" *
        fi
        
        echo -e "\e[92mZipped everything to $BUILD_DIR/${BUILD_TARGET}-tools-[windows | linux | macos].zip\e[39m"
    else
        echoColor "    Skipping zipping 'zip' was ommitted from commandline args '$args'"
    fi
}

function echoColor {
    echo -e "\e[96m$1\e[39m"
}

function echoError {
    echo -e "\e[31m$1\e[39m"
}

main