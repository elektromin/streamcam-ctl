#!/bin/bash
# exp.sh — find which Logitech XU control drives exposure, by poking and observing.
cd "$(dirname "$0")"

readexp()  { ./streamcam-ctl-legacy status 2>/dev/null | grep "Exposure time absolute" | sed -E 's/.*: ([0-9]+) .*/\1/'; }
readgain() { ./streamcam-ctl-legacy status 2>/dev/null | grep "^\s*Gain" | sed -E 's/.*: ([0-9]+) .*/\1/'; }
hexval()   { ./xuctl "$@" get 2>/dev/null | sed -E 's/.*=(.*)/\1/'; }

revhex() { # little-endian hex -> decimal
  local h=$1 out=0
  for ((i=${#h}-2; i>=0; i-=2)); do out=$(( out*256 + 16#${h:$i:2} )); done
  echo $out
}
dechex() { # decimal -> little-endian hex with len bytes
  local v=$1 len=$2 out=""
  for ((i=0; i<len; i++)); do out+="$(printf %02x $(( (v >> (8*i)) & 255 )))"; done
  echo $out
}

E0=$(readexp); G0=$(readgain)
echo "baseline: exposure=$E0 gain=$G0"

probe() { # unit sel len
  local unit=$1 sel=$2 len=$3
  local orig=$(hexval $unit $sel $len)
  [ -z "$orig" ] && { echo "unit $unit sel $sel: GET failed, skipping"; return; }
  local oval=$(revhex $orig)
  local maxv=$(( len==1 ? 255 : len==2 ? 65535 : len==4 ? 2147483647 : 0 ))
  local delta=$(( oval < 16 ? 1 : oval/20 + 1 ))
  local nval=$(( oval + delta ))
  [ $nval -gt $maxv ] && nval=$(( oval - delta ))
  [ $nval -lt 0 ] && nval=0
  ./xuctl $unit $sel $len set $(dechex $nval $len) >/dev/null 2>&1
  sleep 0.8
  local e=$(readexp) g=$(readgain)
  local mark=""
  [ "$e" != "$E0" ] && mark+=" <-- EXPOSURE CHANGED"
  [ "$g" != "$G0" ] && mark+=" <-- GAIN CHANGED"
  echo "unit $unit sel $sel: $oval -> $nval : exposure=$e gain=$g$mark"
  ./xuctl $unit $sel $len set $(dechex $oval $len) >/dev/null 2>&1
  sleep 0.4
}

# main Logitech control XU (unit 9)
for spec in "1 2" "2 1" "3 2" "4 1" "5 2" "6 2" "10 1" "11 1" "12 1" "13 1" "14 1" "15 1" "16 1" "17 4"; do
  probe 9 $spec
done
# other candidates
probe 10 2 1
probe 8 3 2

echo "after: exposure=$(readexp) gain=$(readgain)"
