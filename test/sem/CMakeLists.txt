# Opt-in (-DV3COMPAT_SEMANTIC=ON): lhns/avxemu's instruction-form cases, run on the real CPU
# under the client, against the reference recorded on AVX2 silicon. Fetches avxemu at configure time.
include(FetchContent)
# test-suite branch (PR #2). If that branch is rewritten, re-pin to the replacing commit.
set(AVXEMU_COMMIT 8242e2ebfc11a0054fb428676e576e4f30f5cae6)
FetchContent_Declare(avxemu GIT_REPOSITORY https://github.com/lhns/avxemu GIT_TAG ${AVXEMU_COMMIT}
                     SOURCE_SUBDIR no-cmake-project)
FetchContent_MakeAvailable(avxemu)
set(S "${avxemu_SOURCE_DIR}")

add_executable(semhw sem_adapter.c "${S}/test/spec/sem_hw.S" "${S}/test/spec/spec.c" "${S}/test/spec/known.c"
               "${S}/test/refdigest.c" "${S}/test/cpu.c")
target_include_directories(semhw PRIVATE "${S}/test/spec" "${S}/src" "${S}/test")
target_compile_options(semhw PRIVATE -UNDEBUG -w)
target_compile_definitions(semhw PRIVATE SEM_MANIFEST="" SEM_KNOWN="")

# 0 match; 3 prints "EMULATION MISSING" (expected until emulation exists on an AVX-only CPU);
# any other failure is a result mismatch.
add_test(NAME semantic-hw COMMAND ${RUN} -- $<TARGET_FILE:semhw>
         "${S}/test/spec/manifest.tsv" "${S}/test/reference/semantics.ref" smoke "${CMAKE_CURRENT_SOURCE_DIR}/known-dr.txt")
set_tests_properties(semantic-hw PROPERTIES TIMEOUT 900 LABELS semantic)
# Control: the same cases without DynamoRIO, to tell client-induced differences from the host's.
add_test(NAME semantic-hw-native COMMAND ${CMAKE_COMMAND} -E env V3COMPAT_NATIVE=1 ${RUN} -- $<TARGET_FILE:semhw>
         "${S}/test/spec/manifest.tsv" "${S}/test/reference/semantics.ref" smoke)
set_tests_properties(semantic-hw-native PROPERTIES TIMEOUT 900 LABELS semantic)
