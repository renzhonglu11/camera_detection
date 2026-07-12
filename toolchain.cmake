set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross-compilation from x86_64 Linux to ARM64 (aarch64) Raspberry Pi.


# =============================================================================
# 1. Compiler Configuration
# =============================================================================
set(TOOLCHAIN_ROOT "/home/rz/Learning/Raspberry_Pi/arm-gnu-toolchain-12.3.rel1-x86_64-aarch64-none-linux-gnu")
set(CMAKE_C_COMPILER   "${TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-g++")

# =============================================================================
# 2. Sysroot Configuration
# =============================================================================
set(CMAKE_SYSROOT "/home/rz/Learning/Raspberry_Pi/rpi-sysroot")

# =============================================================================
# 3. FIX: Multiarch Flags (The Critical Part)
# =============================================================================
# Define the arch-specific folders inside your sysroot
set(RPI_ARCH_LIB     "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(RPI_ARCH_INC     "${CMAKE_SYSROOT}/usr/include/aarch64-linux-gnu")
set(RPI_LOWER_LIB    "${CMAKE_SYSROOT}/lib/aarch64-linux-gnu")

# NEW: Add /usr/local paths
set(RPI_LOCAL_LIB    "${CMAKE_SYSROOT}/usr/local/lib")
set(RPI_LOCAL_INC    "${CMAKE_SYSROOT}/usr/local/include")

# Flags explanation:
# -I : Tells compiler where to find headers (fixes bits/wordsize.h error)
# -B : Tells compiler where to find startup objects (crt1.o)
# -L : Tells linker where to find libraries (libm.so, libc.so)
set(MULTIARCH_FLAGS "-I${RPI_ARCH_INC} -I${RPI_LOCAL_INC} -B${RPI_ARCH_LIB} -B${RPI_LOWER_LIB} -L${RPI_ARCH_LIB} -L${RPI_LOWER_LIB} -L${RPI_LOCAL_LIB}")

# Force these flags into the build variables
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   ${MULTIARCH_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MULTIARCH_FLAGS}" CACHE STRING "" FORCE)

# =============================================================================
# 4. Search Modes
# =============================================================================
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)