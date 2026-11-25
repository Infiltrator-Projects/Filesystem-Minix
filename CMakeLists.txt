cmake_minimum_required(VERSION 3.20)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MINIX_VERSION)
string(STRIP "${MINIX_VERSION}" MINIX_VERSION)
project(FilesystemMinix VERSION "${MINIX_VERSION}" LANGUAGES C)
include(CTest)
include(FetchContent)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
 GIT_TAG 965e5d191a6f7d078f5d07218dbaabfb00fa1a20)
FetchContent_MakeAvailable(infiltratr_common)
# Common is a link-time dependency; its development files must not be installed
# by this standalone filesystem package.
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
# The independent firmware profile compiles the same Minix V1/V2/V3
# native disk reader and Common V2 callbacks as the hosted package.
set(MINIX_FIRMWARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/firmware")
set(MINIX_FIRMWARE_ELF "${MINIX_FIRMWARE_DIR}/MINIX.so")
add_custom_command(OUTPUT "${MINIX_FIRMWARE_ELF}"
 COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/module/firmware"
  -B "${MINIX_FIRMWARE_DIR}" -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCOMMON_INCLUDE_DIR=${infiltratr_common_SOURCE_DIR}/include"
 COMMAND "${CMAKE_COMMAND}" --build "${MINIX_FIRMWARE_DIR}" --parallel 2
 DEPENDS module/firmware/CMakeLists.txt module/firmware/runtime.c
  module/minix_common.c native/filesystems/minix/module/minix_module.c
  "${infiltratr_common_SOURCE_DIR}/include/infiltratr/filesystem_module_v2.h"
 VERBATIM)
add_custom_target(minix-firmware-elf DEPENDS "${MINIX_FIRMWARE_ELF}")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"${PROJECT_VERSION}\"\n")
# Production ships Common V2 and the canonical source-owned engine only.
add_library(minix-driver SHARED
 native/filesystems/minix/module/minix_module.c
 module/minix_common.c)
set_source_files_properties(native/minix_worker.c PROPERTIES COMPILE_DEFINITIONS "main=ifs_minix_worker_main")
set_source_files_properties(fixtures/minix_codec.c PROPERTIES COMPILE_DEFINITIONS "ldtm_raw_minix=ifs_minix_raw_fixture")
set_target_properties(minix-driver PROPERTIES PREFIX "" OUTPUT_NAME "MINIX" SUFFIX ".so")
add_dependencies(minix-driver minix-firmware-elf)
set_property(TARGET minix-driver APPEND PROPERTY LINK_DEPENDS "${MINIX_FIRMWARE_ELF}")
add_custom_command(TARGET minix-driver POST_BUILD
 COMMAND "${CMAKE_OBJCOPY}" --add-section
  ".infiltrator_boot=${MINIX_FIRMWARE_ELF}"
  --set-section-flags .infiltrator_boot=readonly
  "$<TARGET_FILE:minix-driver>"
 VERBATIM)
target_compile_definitions(minix-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(minix-driver PRIVATE
 -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)
target_link_options(minix-driver PRIVATE
 -Wl,-z,defs -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map)
target_link_options(minix-driver PRIVATE -Wl,-z,defs)
target_include_directories(minix-driver PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
 "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/minix/module"
 "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(minix-driver PRIVATE InfiltratrCommon::Common)
if(BUILD_TESTING)
 add_executable(minix-native-test
   tests/test_minix_native.c native/minix_native.c tests/generic_test_io.c)
 target_include_directories(minix-native-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/native"
   "${CMAKE_CURRENT_SOURCE_DIR}/compat")
 target_compile_definitions(minix-native-test PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_link_libraries(minix-native-test PRIVATE InfiltratrCommon::Common)
 add_test(NAME minix-original-native-and-fixtures COMMAND minix-native-test)
 add_executable(minix-common-v2-media-test tests/minix_dropin_module_test.c
   native/filesystems/minix/module/minix_module.c module/minix_common.c)
 target_include_directories(minix-common-v2-media-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/minix/module")
 target_link_libraries(minix-common-v2-media-test PRIVATE InfiltratrCommon::Common)
 target_compile_definitions(minix-common-v2-media-test PRIVATE IFS_TEST_COMMON_V2=1)
 add_test(NAME minix-common-v2-disk-variants COMMAND minix-common-v2-media-test)
 add_test(NAME minix-production-v2-only
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_production_v2.sh"
          "$<TARGET_FILE:minix-driver>")
endif()
install(TARGETS minix-driver LIBRARY DESTINATION /System/Filesystems)
# Runtime payload: one filesystem shared object and its single PDF manual.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/Minix.pdf"
        DESTINATION /System/Resources/Documentation RENAME Filesystem-Minix.pdf)
