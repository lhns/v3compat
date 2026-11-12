# DynamoRIO is built from our fork at a pinned commit at configure time (ADR 0003), release config only.
# V3COMPAT_DR_ROOT points at an existing install instead (offline builds).
set(V3COMPAT_DR_REPO https://github.com/lhns/dynamorio)
set(V3COMPAT_DR_COMMIT 8c9413b247a8c219c377c573cfbfbbc18d1d43ff)
set(V3COMPAT_DR_ROOT "" CACHE PATH "Use this DynamoRIO install instead of building the pinned commit")
# Outside the build tree so a fresh build dir does not rebuild it.
set(V3COMPAT_DR_CACHE "$ENV{HOME}/.cache/v3compat" CACHE PATH "DynamoRIO source build cache")

function(_v3compat_run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc)
  if(_rc)
    message(FATAL_ERROR "DynamoRIO build step failed (${_rc}): ${ARGN}")
  endif()
endfunction()

if(NOT V3COMPAT_DR_ROOT)
  set(V3COMPAT_DR_ROOT "${V3COMPAT_DR_CACHE}/dr-${V3COMPAT_DR_COMMIT}")
  if(NOT EXISTS "${V3COMPAT_DR_ROOT}/.done")
    message(STATUS "Building DynamoRIO ${V3COMPAT_DR_COMMIT}")
    set(_work "${V3COMPAT_DR_ROOT}.work")
    file(REMOVE_RECURSE "${V3COMPAT_DR_ROOT}" "${_work}")
    file(MAKE_DIRECTORY "${_work}/src")
    _v3compat_run(git -C "${_work}/src" init -q)
    _v3compat_run(git -C "${_work}/src" fetch -q --depth 1 ${V3COMPAT_DR_REPO} ${V3COMPAT_DR_COMMIT})
    _v3compat_run(git -C "${_work}/src" checkout -q FETCH_HEAD)
    # drsyms, built with the extensions, is the only part needing a submodule.
    _v3compat_run(git -C "${_work}/src" submodule update --init --depth 1 third_party/elfutils)
    _v3compat_run(${CMAKE_COMMAND} -S "${_work}/src" -B "${_work}/build"
      # DynamoRIO's checks want gas itself, and newer CMake picks the compiler driver.
      -DCMAKE_ASM_COMPILER=as -DCMAKE_INSTALL_PREFIX=${V3COMPAT_DR_ROOT} -DDEBUG=OFF -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF
      -DBUILD_SAMPLES=OFF -DBUILD_CLIENTS=OFF)
    include(ProcessorCount)
    ProcessorCount(_jobs)
    _v3compat_run(${CMAKE_COMMAND} --build "${_work}/build" --target install --parallel ${_jobs})
    file(WRITE "${V3COMPAT_DR_ROOT}/.done" "")
    file(REMOVE_RECURSE "${_work}")
  endif()
endif()
set(DynamoRIO_DIR "${V3COMPAT_DR_ROOT}/cmake" CACHE PATH "" FORCE)
