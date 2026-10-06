#!/bin/bash
# PrototypeRecomp Phase 2C — compile all generated guest TUs at -O0 into
# phase2_build (parallel), for the debug runtime.
source /home/z/my-project/scripts/env.sh
set -e
PPC=/home/z/my-project/analysis/out/phase2
BUILD=/home/z/my-project/analysis/out/phase2_build
XR=/home/z/my-project/scripts/research/xenonrecomp
LOG=/home/z/my-project/scripts/PrototypeRecomp/analysis/logs/phase2b
mkdir -p "$BUILD"
cd "$PPC"
START=$(date +%s)

compile_one() {
    f="$1"
    obj="$BUILD/${f%.cpp}.o"
    err="$LOG/guest_${f%.cpp}.err"
    clang++ -std=c++17 -O0 -w -c "$f" -I"$PPC" -I"$XR/thirdparty/simde" \
        -o "$obj" 2> "$err"
    if [ -s "$err" ]; then
        echo "WARN in $f:"
        head -5 "$err"
    fi
}
export -f compile_one
export PPC BUILD XR LOG

# Also the mapping TU.
( compile_one "ppc_func_mapping.cpp" ) &

for f in ppc_recomp.*.cpp; do
    obj="$BUILD/${f%.cpp}.o"
    if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
        compile_one "$f" &
        while [ "$(jobs -r | wc -l)" -ge "$(nproc)" ]; do wait -n; done
    fi
done
wait
END=$(date +%s)
FAILS=0
for e in "$LOG"/guest_*.err; do
    [ -f "$e" ] && [ -s "$e" ] && { FAILS=$((FAILS+1)); echo "ERR in $e:"; head -3 "$e"; }
done
OBJS=$(ls "$BUILD"/*.o 2>/dev/null | wc -l)
echo "guest -O0 compile done: objects=$OBJS fails=$FAILS elapsed=$((END-START))s"
