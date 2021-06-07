#!/bin/env bash

outdir="./fuzz2"
outfile='address_sanitizer.log'
peakmemory='1G'
peakcpu='200%'

function limit_resources() {
    systemd-run --scope -p MemoryHigh=${peakmemory} -p MemoryMax=${peakmemory} -p CPUQuota=${peakcpu} \
    -p MemorySwapMax=infinity -p CPUAccounting=1 -p MemoryAccounting=1 -p TasksAccounting=1 $1
}

# Respawn process on crash

# limit_resources "xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer00 -b 0 ./fuzz.out" &
# limit_resources "xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer01 -b 1 ./fuzz.out" &
# limit_resources "xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer02 -b 2 ./fuzz.out" &
# limit_resources "xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer03 -b 3 ./fuzz.out" &
# limit_resources "xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer04 -b 4 ./fuzz.out" &
# limit_resources "xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer05 -b 5 ./fuzz.out" &
# limit_resources "xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer06 -b 6 ./fuzz.out" &
# limit_resources "xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer07 -b 7 ./fuzz.out" &

xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer00 ./fuzz.out &
xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer01 ./fuzz.out &
# xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer02 -b 2 ./fuzz.out &
# xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer03 -b 3 ./fuzz.out &
# xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer04 -b 4 ./fuzz.out &
# xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer05 -b 5 ./fuzz.out &
# xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer06 -b 6 ./fuzz.out &
# xfce4-terminal -x `which afl-fuzz` -i testcase -o ${outdir} -M fuzzer07 -b 7 ./fuzz.out &

bash ./respawn_pidgin.sh
