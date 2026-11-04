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
add_executable(fault_state fault_state.c)
add_executable(flags_kept flags_kept.c)
add_executable(misc_forms misc_forms.c)
add_executable(refuse_forms refuse_forms.c)
add_executable(approx approx.c)
target_link_libraries(approx PRIVATE m)
target_compile_options(approx PRIVATE -O1 -ffp-contract=off)
add_library(lzlib SHARED lzlib.c)
target_compile_options(lzlib PRIVATE -mlzcnt)
add_executable(lzcnt_dyn lzcnt.c)
target_link_libraries(lzcnt_dyn PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(lzcnt_dyn PRIVATE -mlzcnt)
add_executable(lzcnt_static lzcnt.c)
target_link_options(lzcnt_static PRIVATE -static)
target_compile_options(lzcnt_static PRIVATE -mlzcnt)
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
# What a fault or a flag reader sees of the application's state, whatever the expansion kind.
v3test(fault-state stdout-has PASS exit 0 -- ${RUN} -- $<TARGET_FILE:fault_state>)
v3test(flags-kept stdout-has PASS exit 0 -- ${RUN} -- $<TARGET_FILE:flags_kept>)
v3test(misc-forms stdout-has PASS exit 0 -- ${RUN} -- $<TARGET_FILE:misc_forms>)
# lzcnt in code the client sees late: a dlopen'd library, and a static program after exec.
v3test(lzcnt-dlopen stdout-has PASS exit 0 -- ${RUN} -- $<TARGET_FILE:lzcnt_dyn> dl $<TARGET_FILE:lzlib>)
v3test(lzcnt-static-exec stdout-has PASS exit 0 -- ${RUN} -- $<TARGET_FILE:lzcnt_dyn> exec $<TARGET_FILE:lzcnt_static>)
add_test(NAME client-stats COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/stats.sh" ${RUN} $<TARGET_FILE:avx2prog> $<TARGET_FILE:hello_dyn>)
set_tests_properties(hello-dynamic hello-static exit-code sigterm maps-has-dynamorio
  exec-child-has-dynamorio native-bypass flags-preserved client-stats fault-state flags-kept misc-forms
  lzcnt-dlopen lzcnt-static-exec PROPERTIES TIMEOUT 120)

# ADR 0008: off the audited list exits 86, on it runs, whatever this CPU would do with it.
foreach(f xop-bextr-imm vmpsadbw-ymm vaesenc-ymm vpclmulqdq-ymm rdgsbase wrgsbase rdfsbase tpause umonitor adcx adox
          sha1rnds4 clflushopt clwb rdpid rdseed xsavec movdiri gf2p8mulb kmovw vzeroupper-w1 vmmcall cmpccxadd jz-66 pause-rexb)
  v3test(refused-${f} exit 86 -- ${RUN} -- $<TARGET_FILE:refuse_forms> ${f})
  set_tests_properties(refused-${f} PROPERTIES TIMEOUT 120)
endforeach()
foreach(f pause rep-ret-next endbr64 prefetchw popcnt crc32 aesenc vcvtph2ps rdrand not-executed)
  v3test(listed-${f} stdout-has PASS exit 0 -- ${RUN} -- $<TARGET_FILE:refuse_forms> ${f})
  set_tests_properties(listed-${f} PROPERTIES TIMEOUT 120)
endforeach()
# Approximating forms the list admits (policy kind approx): within the documented or stated error, specials included.
v3test(approx-forms stdout-has PASS exit 0 -- ${RUN} -- $<TARGET_FILE:approx>)
set_tests_properties(approx-forms PROPERTIES TIMEOUT 300)
v3test(cpuid-hides-xsavec stdout-has xsave-extras=0 exit 0 -- ${RUN} -- $<TARGET_FILE:refuse_forms> cpuid-xsavec)
v3test(cpuid-hides-fsgsbase stdout-has fsgsbase=0 exit 0 -- ${RUN} -- $<TARGET_FILE:refuse_forms> cpuid-fsgsbase)

# The list: evidence and generated files agree, DR decodes every row to its key, and this CPU matches the
# Ivy Bridge results on every tested row. allow_probe.tsv is CI's artifact, the AVX2 evidence (regen.sh).
set(_A "${CMAKE_CURRENT_SOURCE_DIR}/allow")
add_executable(allow_probe "${_A}/probe.c" "${_A}/probe_stub.S")
target_compile_options(allow_probe PRIVATE -fno-stack-protector -O1 -U_FORTIFY_SOURCE)
target_link_options(allow_probe PRIVATE -static)
add_test(NAME allowlist-check COMMAND sh -c "gzip -dc '${_A}/evidence/rows.tsv.gz' > rows.tsv && $<TARGET_FILE:allow_dkey> < rows.tsv > keys.tsv && $<TARGET_FILE:allow_probe> < rows.tsv > allow_probe.tsv && python3 '${_A}/gen.py' check allow_probe.tsv keys.tsv")
set_tests_properties(allowlist-check PROPERTIES TIMEOUT 300)

# The client's archive must stay plain AVX1 with no libc: the CI CPU would run anything.
add_test(NAME helpers-guard COMMAND sh "${AVXEMU_SRC}/test/check-helpers-lib.sh" $<TARGET_FILE:avxemu_helpers>)

option(V3COMPAT_SEMANTIC "Add the opt-in semantic suite test (fetches lhns/avxemu)" OFF)
if(V3COMPAT_SEMANTIC)
  add_subdirectory(sem)
endif()
