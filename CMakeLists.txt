cmake_minimum_required(VERSION 3.18)
project(v3compat C ASM)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  message(FATAL_ERROR "v3compat supports Linux x86-64 only")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(cmake/FetchDynamoRIO.cmake)
# DynamoRIO's bundled DrMemoryFramework config reads this, and we enable no C++.
set(CMAKE_CXX_SIZEOF_DATA_PTR ${CMAKE_C_SIZEOF_DATA_PTR})
find_package(DynamoRIO REQUIRED)

# lhns/avxemu's emulation core: its archive's flags, checked by its guard script (test/CMakeLists.txt).
include(FetchContent)
set(AVXEMU_COMMIT 836323eb8367c7cd2b3c11bfb155d92e4c3654ca)
FetchContent_Declare(avxemu GIT_REPOSITORY https://github.com/lhns/avxemu GIT_TAG ${AVXEMU_COMMIT}
                     SOURCE_SUBDIR no-cmake-project)
FetchContent_MakeAvailable(avxemu)
set(AVXEMU_SRC "${avxemu_SOURCE_DIR}")
add_library(avxemu_helpers STATIC "${AVXEMU_SRC}/src/exec.c" "${AVXEMU_SRC}/src/exec_bmi.c" "${AVXEMU_SRC}/src/softfma.c")
target_include_directories(avxemu_helpers PUBLIC "${AVXEMU_SRC}/src")
target_compile_options(avxemu_helpers PRIVATE -O2 -std=c11 -w -mavx -mno-avx2 -mno-fma -mno-f16c -mno-bmi -mno-bmi2
  -mno-lzcnt -mno-movbe -mtune=ivybridge -fPIC -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)

add_library(v3compat SHARED src/client.c src/expand.c)
target_link_libraries(v3compat avxemu_helpers)
configure_DynamoRIO_client(v3compat)
# drmgr is BSD; its shared build is installed beside the client (the static one pulls in fortified libc calls DR cannot resolve).
use_DynamoRIO_extension(v3compat drmgr)
set_target_properties(v3compat PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
# DR exports only a few libc functions; a fortified memcpy would not resolve.
target_compile_options(v3compat PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-builtin)
set_target_properties(v3compat PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libexec/v3compat")

# DynamoRIO pieces the launcher needs, under libexec/v3compat/dynamorio.
set(DR_RUNTIME_DIR libexec/v3compat/dynamorio)
configure_file(launcher/v3compat-run "${CMAKE_BINARY_DIR}/bin/v3compat-run" COPYONLY)

include(GNUInstallDirs)
install(TARGETS v3compat LIBRARY DESTINATION libexec/v3compat)
install(PROGRAMS "${V3COMPAT_DR_ROOT}/ext/lib64/release/libdrmgr.so" DESTINATION libexec/v3compat)
install(PROGRAMS launcher/v3compat-run DESTINATION bin)
install(PROGRAMS "${V3COMPAT_DR_ROOT}/bin64/drrun" DESTINATION ${DR_RUNTIME_DIR}/bin64)
install(PROGRAMS "${V3COMPAT_DR_ROOT}/lib64/release/libdynamorio.so" "${V3COMPAT_DR_ROOT}/lib64/release/libdrpreload.so"
        DESTINATION ${DR_RUNTIME_DIR}/lib64/release)
install(FILES LICENSE DESTINATION share/doc/v3compat)
install(FILES "${V3COMPAT_DR_ROOT}/License.txt" DESTINATION share/doc/v3compat RENAME LICENSE.dynamorio)
install(FILES README.md DESTINATION share/doc/v3compat)

# Release-style tarball: `cmake --build . --target package-tarball`.
set(V3COMPAT_VERSION 0.0.0)
set(_stage "${CMAKE_BINARY_DIR}/stage/v3compat-${V3COMPAT_VERSION}-linux-x86_64")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/stage")
add_custom_target(package-tarball
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${_stage}"
  COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${_stage}"
  COMMAND ${CMAKE_COMMAND} -E tar czf "${CMAKE_BINARY_DIR}/v3compat-${V3COMPAT_VERSION}-linux-x86_64.tar.gz" "v3compat-${V3COMPAT_VERSION}-linux-x86_64"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/stage"
  DEPENDS v3compat VERBATIM)

enable_testing()
add_subdirectory(test)
