#!/bin/sh
for nm in TLH TRH TLL TRL BLH BRH BLL BRL; do
  amixer -c0 cset "name=$nm PCM Source" DSP >/dev/null 2>&1
done
echo "== 校验 =="
for nm in TLH BLH BRH; do
  n=$(amixer -c0 controls 2>/dev/null | grep "name='$nm PCM Source'" | grep -oE "numid=[0-9]+" | cut -d= -f2)
  echo "   $nm = $(amixer -c0 cget numid=$n 2>/dev/null | sed -n '4p' | tr -d ' ') $(amixer -c0 cget numid=$n 2>/dev/null | sed -n '3p' | tr -d ' ')"
done
