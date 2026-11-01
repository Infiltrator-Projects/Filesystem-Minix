cmake_minimum_required(VERSION 3.20)
project(FilesystemMinix VERSION 1.0.0 LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
 GIT_TAG c76ec95ef66fc728df6a906ad75a6a4dd63b419b)
FetchContent_MakeAvailable(infiltratr_common)
# Common is a link-time dependency; its development files must not be installed
# by this standalone filesystem package.
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"1.0.0\"\n")
add_library(minix-driver SHARED native/minix_native.c native/minix_worker.c
 native/filesystems/minix/module/minix_module.c
 compat/legacy_services.c module/minix_defrag_module.c
 fixtures/minix_codec.c module/minix_fixture_bridge.c)
set_source_files_properties(native/minix_worker.c PROPERTIES COMPILE_DEFINITIONS "main=ifs_minix_worker_main")
set_source_files_properties(fixtures/minix_codec.c PROPERTIES COMPILE_DEFINITIONS "ldtm_raw_minix=ifs_minix_raw_fixture")
set_target_properties(minix-driver PROPERTIES PREFIX "" OUTPUT_NAME "MINIX" SUFFIX ".so")
target_compile_definitions(minix-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(minix-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(minix-driver PRIVATE -Wl,-z,defs)
target_include_directories(minix-driver PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
 "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/minix/module"
 "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(minix-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)
if(BUILD_TESTING)
 add_executable(minix-defrag-test tests/test_defrag_module.c)
 target_include_directories(minix-defrag-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
 target_link_libraries(minix-defrag-test PRIVATE minix-driver)
 add_test(NAME minix-defrag-interface COMMAND minix-defrag-test)
 add_executable(minix-native-test
   tests/test_minix_native.c native/minix_native.c tests/generic_test_io.c)
 target_include_directories(minix-native-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/native"
   "${CMAKE_CURRENT_SOURCE_DIR}/compat")
 target_compile_definitions(minix-native-test PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_link_libraries(minix-native-test PRIVATE InfiltratrCommon::Common)
 add_test(NAME minix-original-native-and-fixtures COMMAND minix-native-test)
 add_executable(minix-v1-v2-module-test tests/minix_dropin_module_test.c
   native/filesystems/minix/module/minix_module.c)
 target_include_directories(minix-v1-v2-module-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/minix/module")
 add_test(NAME minix-original-v1-v2 COMMAND minix-v1-v2-module-test)
endif()
install(TARGETS minix-driver LIBRARY DESTINATION /System/Filesystems)
# Runtime payload: one filesystem shared object and its single PDF manual.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/Minix.pdf"
        DESTINATION /System/Resources/Documentation/Filesystem-Minix)
