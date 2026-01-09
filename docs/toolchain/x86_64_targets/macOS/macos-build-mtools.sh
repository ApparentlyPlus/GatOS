export PATH=~/Desktop/osxcross/target/bin:$PATH

# 1. Define the target
export TARGET_ARM=aarch64-apple-darwin23.5

# 2. Set environment variables for the compiler
export CC=$TARGET_ARM-clang
export CXX=$TARGET_ARM-clang++
export AR=$TARGET_ARM-ar
export RANLIB=$TARGET_ARM-ranlib
export STRIP=$TARGET_ARM-strip

# 3. Create a build directory
mkdir build-arm
cd build-arm

# 4. Run configure, telling it the "host" system we are building for
../configure --host=$TARGET_ARM

# 5. Build the software
make

# 1. Go back to the source root
cd ..

# 2. Define the target
export TARGET_INTEL=x86_64-apple-darwin23.5

# 3. Set environment variables for the compiler
export CC=$TARGET_INTEL-clang
export CXX=$TARGET_INTEL-clang++
export AR=$TARGET_INTEL-ar
export RANLIB=$TARGET_INTEL-ranlib
export STRIP=$TARGET_INTEL-strip

# 4. Create a separate build directory
mkdir build-intel
cd build-intel

# 5. Run configure for the Intel target
../configure --host=$TARGET_INTEL

# 6. Build the software
make

# 1. Go back to the source root
cd ..

# 2. Use lipo to create the universal binary
lipo -create -output mtools-universal \
     build-arm/mtools \
     build-intel/mtools