export PATH=~/Desktop/osxcross/target/bin:$PATH

# 1. Define the target
export TARGET_ARM=aarch64-apple-darwin23.5

# 2. Set environment variables
export CC=$TARGET_ARM-clang
export CXX=$TARGET_ARM-clang++
# ...set AR, RANLIB, STRIP as before...

# 3. Create a build directory
mkdir build-arm
cd build-arm

# 4. Run configure
# NOTE: You may need to add CFLAGS/LDFLAGS to point to your
# cross-compiled dependencies if they aren't found automatically.
../configure --host=$TARGET_ARM

# 5. Build
make

# 1. Go back to the source root
cd ..

# 2. Define the target
export TARGET_INTEL=x86_64-apple-darwin23.5

# 3. Set environment variables
export CC=$TARGET_INTEL-clang
export CXX=$TARGET_INTEL-clang++
# ...set AR, RANLIB, STRIP as before...

# 4. Create a build directory
mkdir build-intel
cd build-intel

# 5. Run configure
../configure --host=$TARGET_INTEL

# 6. Build
make

# 1. Go back to the source root
cd ..

# 2. Use lipo to create the universal binary
lipo -create -output xorriso-universal \
     build-arm/xorriso/xorriso \
     build-intel/xorriso/xorriso