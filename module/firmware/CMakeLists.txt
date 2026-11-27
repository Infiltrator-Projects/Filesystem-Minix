# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(MinixBootReader LANGUAGES C)
set(COMMON_INCLUDE_DIR "" CACHE PATH "Pinned Common V2 header root")
if(NOT EXISTS "${COMMON_INCLUDE_DIR}/infiltratr/filesystem_module_v2.h")
 message(FATAL_ERROR "Set COMMON_INCLUDE_DIR to the exact Common V2 headers")
endif()
add_library(minix-boot MODULE
 ../../module/minix_common.c
 ../../native/filesystems/minix/module/minix_module.c
 runtime.c)
set_target_properties(minix-boot PROPERTIES
 PREFIX "" OUTPUT_NAME "MINIX" SUFFIX ".so" C_VISIBILITY_PRESET hidden)
target_compile_features(minix-boot PRIVATE c_std_11)
target_include_directories(minix-boot PRIVATE "${COMMON_INCLUDE_DIR}")
target_compile_definitions(minix-boot PRIVATE IFS_MINIX_FIRMWARE_BUILD=1)
# Firmware uses AMD64 SysV without CET, SIMD state assumptions,
# packed relative relocations or non-4-KiB load alignment. Hosted builds
# retain their separate native optimization profile.
target_compile_options(minix-boot PRIVATE
 -Wall -Wextra -Wpedantic -Werror -ffreestanding -fno-builtin
 -fno-tree-loop-distribute-patterns -fno-stack-protector -mno-red-zone -fcf-protection=none -mgeneral-regs-only
 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(minix-boot PRIVATE
 -nostdlib -Wl,-z,nopack-relative-relocs,-z,max-page-size=4096,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now)
