#!/bin/sh
# Smallest LVGL heap that renders every screen and state exactly as the
# configured one does. Rebuilds only the two allocator files with
# VOIDOS_LV_MEM_SIZE=N, relinks snap and renders all shots; a size passes
# when nothing asserts (LVGL halts on a failed malloc: fault_blink 7) and
# every PPM is byte-identical to the reference run.
# usage: heapcheck.sh [from_bytes] [to_bytes] [step]
set -e
cd "$(dirname "$0")"
FW=../../firmware
CFLAGS="-m32 -O1 -g -std=gnu11 -w -DLV_CONF_INCLUDE_SIMPLE -DLV_USE_OS=LV_OS_NONE \
  -I$FW/vendor -I$FW/vendor/lvgl -I$FW -I../../include -I../../core -I$FW/nimble_port/include"
FROM=${1:-6144}; TO=${2:-20480}; STEP=${3:-512}
make -s snap
W=$(mktemp -d)
mkdir -p $W/ref && ./snap $W/ref > $W/ref.log
grep "LVGL heap" $W/ref.log
OBJS=$(find build/vendor -name '*.o' ! -name lv_tlsf.o ! -name lv_mem_core_builtin.o)
N=$FROM
while [ $N -le $TO ]; do
  for f in lv_tlsf lv_mem_core_builtin; do
    cc $CFLAGS -DVOIDOS_LV_MEM_SIZE=$N -c -o $W/$f.o $FW/vendor/lvgl/src/stdlib/builtin/$f.c
  done
  cc $CFLAGS -o $W/snap snap.c $FW/ui_face.c $OBJS $W/lv_tlsf.o $W/lv_mem_core_builtin.o \
     build/gen/kenney_lvgl.o build/gen/assets_data.o -lm
  rm -rf $W/out; mkdir -p $W/out
  if $W/snap $W/out > $W/out.log 2>&1 && \
     for p in $W/ref/*.ppm; do cmp -s $p $W/out/$(basename $p) || exit 1; done; then
    echo "smallest heap that renders everything identically: $N B"
    rm -rf $W; exit 0
  fi
  echo "$N B: fails ($(grep -m1 -o 'fault_blink [0-9]*' $W/out.log || echo 'pixels differ'))"
  N=$((N + STEP))
done
echo "nothing up to $TO B passed"; rm -rf $W; exit 1
