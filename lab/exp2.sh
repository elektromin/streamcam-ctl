#!/bin/bash
# exp2.sh — oracle: CT exposure (E), PU gain (G), PU WB temp (W). Poke XU candidates with
# distinctive values (exposure-like 500, WB-like 3000) and see which register moves.
cd "$(dirname "$0")"

snap() {
  ./streamcam-ctl-legacy status 2>/dev/null | awk '
    /Exposure time absolute/ {e=$4}
    /^\s*Gain/               {g=$4}
    /WB temperature \(K\)/   {w=$4}
    END {printf "E=%s G=%s W=%s", e, g, w}'
}
hexval() { ./xuctl "$@" get 2>/dev/null | sed -E 's/.*=(.*)/\1/'; }
revhex() { local h=$1 out=0; for ((i=${#h}-2; i>=0; i-=2)); do out=$(( out*256 + 16#${h:$i:2} )); done; echo $out; }
dechex() { local v=$1 len=$2 out=""; for ((i=0; i<len; i++)); do out+="$(printf %02x $(( (v >> (8*i)) & 255 )))"; done; echo $out; }

echo "wbauto off:"; ./streamcam-ctl-legacy set-wbauto off; sleep 0.5
echo "before: $(snap)"

poke() { # unit sel len value label
  local unit=$1 sel=$2 len=$3 val=$4
  local orig=$(hexval $unit $sel $len)
  local oval=""; [ -n "$orig" ] && oval=$(revhex $orig)
  ./xuctl $unit $sel $len set $(dechex $val $len) >/dev/null 2>&1
  sleep 0.8
  echo "  poke u$unit s$sel ← $val : $(snap)"
  if [ -n "$orig" ]; then ./xuctl $unit $sel $len set $(dechex $oval $len) >/dev/null 2>&1; fi
  sleep 0.3
}

echo "-- wb target 3000 candidates --"
for spec in "9 1 2 3000" "9 3 2 3000" "9 5 2 3000" "9 6 2 3000" "8 3 2 3000" "14 3 3 3000" "14 4 3 3000" "14 5 3 3000"; do
  poke $spec
done
echo "after wb pokes: $(snap)"

echo "-- exposure target 500 candidates --"
for spec in "9 1 2 500" "9 3 2 500" "9 5 2 500" "9 6 2 500" "8 3 2 500"; do
  poke $spec
done
echo "after exp pokes: $(snap)"

echo "-- gain target 100 candidates (1-byte) --"
for spec in "9 2 1 100" "9 4 1 100" "9 10 1 1" "9 11 1 3" "9 12 1 1" "9 13 1 1" "9 14 1 1" "9 15 1 32" "9 16 1 0" "10 2 1 1"; do
  poke $spec
done
echo "after gain/mode pokes: $(snap)"
