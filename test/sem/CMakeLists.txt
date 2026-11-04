# Opt-in (-DV3COMPAT_SEMANTIC=ON): lhns/avxemu's instruction-form cases, run on the real CPU
# under the client, against the reference recorded on AVX2 silicon. Fetches avxemu at configure time.
set(S "${AVXEMU_SRC}")

add_executable(semhw sem_adapter.c "${S}/test/spec/sem_hw.S" "${S}/test/spec/spec.c" "${S}/test/spec/known.c"
               "${S}/test/refdigest.c" "${S}/test/cpu.c")
target_include_directories(semhw PRIVATE "${S}/test/spec" "${S}/src" "${S}/test")
target_compile_options(semhw PRIVATE -UNDEBUG -w)
target_compile_definitions(semhw PRIVATE SEM_MANIFEST="" SEM_KNOWN="")

# Under the client, with and without inline expansions (stage 4: both must match the references).
# refused.txt lists the forms still refused; it has to stay exact.
set(_sem_args "${S}/test/spec/manifest.tsv" "${S}/test/reference/semantics.ref" smoke)
add_test(NAME semantic-hw COMMAND ${RUN} -- $<TARGET_FILE:semhw> ${_sem_args} "${CMAKE_CURRENT_SOURCE_DIR}/refused.txt")
add_test(NAME semantic-hw-helpers COMMAND ${CMAKE_COMMAND} -E env V3COMPAT_HELPERS_ONLY=1 ${RUN} -- $<TARGET_FILE:semhw>
         ${_sem_args} "${CMAKE_CURRENT_SOURCE_DIR}/refused.txt")
# Control on an AVX2 host: the same cases without DynamoRIO, to tell client differences from the host's.
add_test(NAME semantic-hw-native COMMAND ${CMAKE_COMMAND} -E env V3COMPAT_NATIVE=1 ${RUN} -- $<TARGET_FILE:semhw> ${_sem_args})
set_tests_properties(semantic-hw semantic-hw-helpers semantic-hw-native PROPERTIES TIMEOUT 900 LABELS semantic)
