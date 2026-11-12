# loop NAME RUNS TIMEOUT cmd...: run repeatedly, classify, dump the first hangs. Counts on stdout.
loop() {
  name=$1; N=$2; T=$3; shift 3; bad=0; hang=0; crash=0; wrong=0; dumps=0; i=0; codes=""
  while [ "$i" -lt "$N" ]; do i=$((i+1))
    st=$(date +%s%N)
    "$@" >lab-out/$name.out 2>lab-out/$name.err &
    pid=$!; t=0
    while kill -0 $pid 2>/dev/null && [ $t -lt $((T*5)) ]; do sleep 0.2; t=$((t+1)); done
    if kill -0 $pid 2>/dev/null; then
      hang=$((hang+1)); bad=$((bad+1))
      if [ $dumps -lt 4 ]; then dumps=$((dumps+1)); { sudo python3 lab/procdump.py $pid; for k in $(pgrep -P $pid); do sudo python3 lab/procdump.py $k; done; } > lab-out/$name.hang$dumps.txt 2>>lab-out/$name.dumperr; [ $dumps = 1 ] && cat lab-out/$name.hang1.txt; cp lab-out/$name.out lab-out/$name.hang$dumps.out; fi
      kill -9 $pid 2>/dev/null; pkill -9 -P $pid 2>/dev/null; wait $pid 2>/dev/null
      echo "$name run $i: hang"
    else
      wait $pid; rc=$?
      ms=$(( ($(date +%s%N) - st) / 1000000 )); [ $ms -gt ${maxms:-0} ] && maxms=$ms
      [ $ms -gt 5000 ] && echo "$name run $i: slow, ${ms} ms"
      if [ $rc != 0 ]; then bad=$((bad+1)); codes="$codes $rc"; echo "$name run $i: rc=$rc"; grep -a -h '@@' lab-out/$name.err | tail -8 | cut -c1-260; cp lab-out/$name.err lab-out/$name.fail$bad.err; fi
    fi
  done
  echo "RESULT $name: $bad of $N bad (hang $hang; codes:$codes; slowest finished run ${maxms:-0} ms)"; maxms=0
}
