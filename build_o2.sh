#!/bin/bash
# PrototypeRecomp Phase 2B — recompile all 331 generated TUs at -O2 for
# execution speed (correctness unchanged; same sources, same includes).
source /home/z/my-project/scripts/env.sh
set -e
PPC=/home/z/my-project/analysis/out/phase2
BUILD=/home/z/my-project/analysis/out/phase2_build_o2
XR=/home/z/my-project/scripts/research/xenonrecomp
LOG=/home/z/my-project/scripts/PrototypeRecomp/analysis/logs/phase2b
mkdir -p "$BUILD" "$LOG/o2_compile"
cd "$PPC"

compile_one() {
    f="$1"
    obj="$BUILD/${f%.cpp}.o"
    err="$LOG/o2_compile/${f%.cpp}.err"
    if clang++ -std=c++17 -O2 -w -c "$f" -I"$PPC" -I"$XR/thirdparty/simde" -o "$obj" 2> "$err"; then
        rm -f "$err"
    else
        echo "FAIL $f"
    }
}
export -f compile_one
export PPC BUILD XR LOG

START=$(date +%s)
ls *.cpp | xargs -P "$(nproc)" -I{} bash -c 'compile_one "$@"' _ {}
END=$(date +%s)

FAILS=$(ls "$LOG/o2_compile"/*.err 2>/dev/null | wc -l)
OBJS=$(ls "$BUILD"/*.o 2>/dev/null | wc -l)
echo "o2_compile: objects=$OBJS fails=$FAILS elapsed=$((END-START))s"
