set(INST "${CMAKE_BINARY_DIR}/inst")
set(RUN "${INST}/bin/v3compat-run")

# Tests run against the installed layout, which is what ships.
add_custom_target(stage ALL COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}" --prefix "${INST}"
                  DEPENDS v3compat VERBATIM)

add_executable(hello_dyn hello.c)
add_executable(hello_static hello.c)
target_link_options(hello_static PRIVATE -static)
add_executable(exitcode exitcode.c)
add_executable(proc proc.c)
add_executable(expect expect.c)
add_executable(avx2prog avx2prog.S)
add_executable(flagrepro flagrepro.c)
target_link_options(avx2prog PRIVATE -nostdlib -static)

function(v3test name)
  add_test(NAME ${name} COMMAND $<TARGET_FILE:expect> ${ARGN})
endfunction()
v3test(hello-dynamic stdout-has hello exit 0 -- ${RUN} -- $<TARGET_FILE:hello_dyn>)
v3test(hello-static stdout-has hello exit 0 -- ${RUN} -- $<TARGET_FILE:hello_static>)
v3test(exit-code exit 42 -- ${RUN} -- $<TARGET_FILE:exitcode>)
v3test(sigterm signal 15 -- ${RUN} -- $<TARGET_FILE:exitcode> term)
v3test(maps-has-dynamorio stdout-has dynamorio-mapped exit 0 -- ${RUN} $<TARGET_FILE:proc> maps)
v3test(exec-child-has-dynamorio stdout-has dynamorio-mapped exit 0 -- ${RUN} $<TARGET_FILE:proc> exec)
v3test(native-bypass stdout-has dynamorio-absent exit 1 -- env V3COMPAT_NATIVE=1 ${RUN} $<TARGET_FILE:proc> maps)
# Flags across block entry, where DynamoRIO's flag table overstates writes.
v3test(flags-preserved stdout-has PASS exit 0 -- ${RUN} -- $<TARGET_FILE:flagrepro>)
add_test(NAME client-stats COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/stats.sh" ${RUN} $<TARGET_FILE:avx2prog> $<TARGET_FILE:hello_dyn>)
set_tests_properties(hello-dynamic hello-static exit-code sigterm maps-has-dynamorio
  exec-child-has-dynamorio native-bypass flags-preserved client-stats PROPERTIES TIMEOUT 120)

# The client's archive must stay plain AVX1 with no libc: the CI CPU would run anything.
add_test(NAME helpers-guard COMMAND sh "${AVXEMU_SRC}/test/check-helpers-lib.sh" $<TARGET_FILE:avxemu_helpers>)

option(V3COMPAT_SEMANTIC "Add the opt-in semantic suite test (fetches lhns/avxemu)" OFF)
if(V3COMPAT_SEMANTIC)
  add_subdirectory(sem)
endif()
