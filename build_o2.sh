#!/bin/bash
# PrototypeRecomp Phase 2B — recompile all 331 generated TUs at -O2.
source /home/z/my-project/scripts/env.sh
PPC=/home/z/my-project/analysis/out/phase2
BUILD=/home/z/my-project/analysis/out/phase2_build_o2
XR=/home/z/my-project/scripts/research/xenonrecomp
LOG=/home/z/my-project/scripts/PrototypeRecomp/analysis/logs/phase2b
mkdir -p "$BUILD"
cd "$PPC"
START=$(date +%s)

for f in *.cpp; do
    obj="$BUILD/${f%.cpp}.o"
    if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
        echo "CXX $f"
        clang++ -std=c++17 -O2 -w -c "$f" -I"$PPC" -I"$XR/thirdparty/simde" -o "$obj" 2> "$LOG/o2_${f%.cpp}.err" &
        # Limit parallelism to nproc.
        while [ "$(jobs -r | wc -l)" -ge "$(nproc)" ]; do wait -n; done
    fi
done
wait
END=$(date +%s)
FAILS=0
for e in "$LOG"/o2_*.err; do
    [ -f "$e" ] && [ -s "$e" ] && { FAILS=$((FAILS+1)); echo "ERR in $e:"; head -5 "$e"; }
done
OBJS=$(ls "$BUILD"/*.o 2>/dev/null | wc -l)
echo "o2_compile done: objects=$OBJS fails=$FAILS elapsed=$((END-START))s"
