#!/bin/bash
# PrototypeRecomp Phase 2B — build the runtime and link against the Phase 2A
# generated objects.
source /home/z/my-project/scripts/env.sh
set -e
RT=/home/z/my-project/scripts/PrototypeRecomp/runtime
PPC=/home/z/my-project/analysis/out/phase2
PPC_BUILD=/home/z/my-project/analysis/out/phase2_build
XR=/home/z/my-project/scripts/research/xenonrecomp
OUT=/home/z/my-project/scripts/PrototypeRecomp/runtime_build
LOG=/home/z/my-project/scripts/PrototypeRecomp/analysis/logs/phase2b
mkdir -p "$OUT" "$LOG"

CXXFLAGS="-std=c++17 -O1 -w -I$RT -I$PPC -I$XR/thirdparty/simde -I$XR/XenonUtils -I$XR/thirdparty/fmt/include -I$XR/thirdparty/tiny-AES-c -pthread"

cd "$RT"
for f in logging state imports_core imports_fs imports_vd imports_xam trace_hooks main; do
    if [ ! -f "$OUT/$f.o" ] || [ "$f.cpp" -nt "$OUT/$f.o" ] || [ "$RT/state.h" -nt "$OUT/$f.o" ] || [ "$RT/guest.h" -nt "$OUT/$f.o" ]; then
        echo "CXX $f.cpp"
        clang++ $CXXFLAGS -c "$f.cpp" -o "$OUT/$f.o" 2> "$LOG/compile_$f.err"
        if [ -s "$LOG/compile_$f.err" ]; then
            echo "--- errors in $f ---"
            head -50 "$LOG/compile_$f.err"
            # abort only if the .o wasn't produced
            [ -f "$OUT/$f.o" ] || exit 1
        fi
    fi
done

echo "LINK"
clang++ -std=c++17 -O1 -rdynamic -pthread \
    "$OUT"/logging.o "$OUT"/state.o "$OUT"/imports_core.o "$OUT"/imports_fs.o \
    "$OUT"/imports_vd.o "$OUT"/imports_xam.o "$OUT"/trace_hooks.o "$OUT"/main.o \
    "$PPC_BUILD"/*.o \
    "$XR/build/XenonUtils/libXenonUtils.a" \
    "$XR/build/thirdparty/fmt/libfmt.a" \
    "$XR/build/thirdparty/disasm/libdisasm.a" \
    "$XR/build/thirdparty/xxHash/cmake_unofficial/libxxhash.a" \
    -o "$OUT/prototype_runtime" 2> "$LOG/link.err"
if [ -s "$LOG/link.err" ]; then
    echo "--- link errors ---"
    head -80 "$LOG/link.err"
    exit 1
fi
echo "BUILD OK: $OUT/prototype_runtime"
