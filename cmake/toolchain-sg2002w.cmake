set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(_recamera_cross_prefix "riscv64-unknown-linux-musl-")
find_program(CMAKE_C_COMPILER NAMES ${_recamera_cross_prefix}gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${_recamera_cross_prefix}g++ REQUIRED)
find_program(CMAKE_AR NAMES ${_recamera_cross_prefix}ar REQUIRED)
find_program(CMAKE_RANLIB NAMES ${_recamera_cross_prefix}ranlib REQUIRED)
find_program(CMAKE_OBJCOPY NAMES ${_recamera_cross_prefix}objcopy REQUIRED)

set(CMAKE_C_FLAGS_INIT "-mcpu=c906fdv -march=rv64gcv0p7_zfh_xthead -mabi=lp64d")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=c906fdv -march=rv64gcv0p7_zfh_xthead -mabi=lp64d")

if(DEFINED ENV{SG200X_SDK_PATH})
    set(_recamera_sdk_root "$ENV{SG200X_SDK_PATH}")
    set(CMAKE_FIND_ROOT_PATH
        "${_recamera_sdk_root}/buildroot-2021.05/output/cvitek_CV181X_musl_riscv64/host/riscv64-buildroot-linux-musl/sysroot")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
