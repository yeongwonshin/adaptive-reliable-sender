#!/usr/bin/env bash
set -u

echo "=============================="
echo "MP1 FULL CHECK START"
echo "=============================="

chmod +x ./netsim 2>/dev/null

echo
echo "[1] Compile"
g++ -O2 -o sender_20211546 sender_20211546.cc netsim_lib.cc
if [ $? -ne 0 ]; then
  echo "COMPILE FAIL"
  exit 1
fi
echo "COMPILE OK"

mkdir -p check_logs check_outputs edge_inputs

make_edge_files() {
  : > edge_inputs/empty.bin
  printf 'A' > edge_inputs/one_byte.bin
  printf 'Hello, MP1 CRC Stop-and-Wait test.\n' > edge_inputs/small_text.txt

  head -c 1024 /dev/urandom > edge_inputs/random_1k.bin
  head -c 65535 /dev/urandom > edge_inputs/random_65535.bin
  head -c 65536 /dev/urandom > edge_inputs/random_65536.bin
  head -c 131071 /dev/urandom > edge_inputs/random_131071.bin
  head -c 65536 /dev/urandom > edge_inputs/high_ber_64k.bin
}

max100() {
  local f="$1"
  local s
  s=$(wc -c < "$f")
  if [ "$s" -eq 0 ]; then
    echo 1000
  else
    echo $((s * 100))
  fi
}

run_case() {
  local name="$1"
  local input="$2"
  local ber="$3"
  local seed="$4"
  local maxbytes="$5"
  local k="${6:-250}"

  local out="check_outputs/${name}.rx"
  local log="check_logs/${name}.log"

  echo
  echo "=============================="
  echo "$name"
  echo "input=$input ber=$ber seed=$seed max_bytes=$maxbytes k=$k"
  echo "=============================="

  ./netsim ./sender_20211546 \
    --input "$input" \
    --output "$out" \
    --ber "$ber" \
    --seed "$seed" \
    --max_bytes "$maxbytes" \
    --k "$k" \
    2> "$log"

  local sim_rc=$?
  local status
  local cost
  local bytes_total
  local frames_total

  status=$(awk -F: '/status/{gsub(/ /,"",$2); print $2}' "$log" | tail -1)
  cost=$(awk -F: '/cost/{gsub(/ /,"",$2); print $2}' "$log" | tail -1)
  bytes_total=$(awk -F: '/bytes_total/{gsub(/ /,"",$2); print $2}' "$log" | tail -1)
  frames_total=$(awk -F: '/frames_total/{gsub(/ /,"",$2); print $2}' "$log" | tail -1)

  if cmp -s "$input" "$out" && [ "$status" = "SUCCESS" ]; then
    echo "$name OK | status=$status | cost=$cost | bytes_total=$bytes_total | frames_total=$frames_total"
    echo "$name,OK,$status,$cost,$bytes_total,$frames_total" >> check_logs/summary.csv
  else
    echo "$name FAIL | sim_rc=$sim_rc | status=$status | cost=$cost"
    echo "---- netsim log ----"
    cat "$log"
    echo "$name,FAIL,$status,$cost,$bytes_total,$frames_total" >> check_logs/summary.csv
  fi
}

echo "case,result,status,cost,bytes_total,frames_total" > check_logs/summary.csv

make_edge_files

echo
echo "[2] Official validation cases"
run_case "validation1" "sherlock_holmes.txt" "1e-6" "1001" "386832300"
run_case "validation2" "cat_bgm.mp3" "1e-5" "2002" "316224000"
run_case "validation3" "cat_bgm.mp3" "1e-4" "3003" "316224000"
run_case "validation4" "harry_potter.txt" "1e-3" "4004" "44276800"

echo
echo "[3] Extra edge/error-risk cases"
run_case "edge_empty" "edge_inputs/empty.bin" "1e-3" "5101" "$(max100 edge_inputs/empty.bin)"
run_case "edge_one_byte" "edge_inputs/one_byte.bin" "1e-3" "5102" "$(max100 edge_inputs/one_byte.bin)"
run_case "edge_small_text" "edge_inputs/small_text.txt" "1e-4" "5103" "$(max100 edge_inputs/small_text.txt)"
run_case "edge_random_1k" "edge_inputs/random_1k.bin" "1e-3" "5104" "$(max100 edge_inputs/random_1k.bin)"
run_case "edge_random_65535" "edge_inputs/random_65535.bin" "1e-5" "5105" "$(max100 edge_inputs/random_65535.bin)"
run_case "edge_random_65536" "edge_inputs/random_65536.bin" "1e-5" "5106" "$(max100 edge_inputs/random_65536.bin)"
run_case "edge_random_131071" "edge_inputs/random_131071.bin" "1e-4" "5107" "$(max100 edge_inputs/random_131071.bin)"
run_case "edge_high_ber_64k" "edge_inputs/high_ber_64k.bin" "1e-3" "5108" "$(max100 edge_inputs/high_ber_64k.bin)"

echo
echo "[4] Optional K variation accuracy check"
run_case "edge_k_1000" "edge_inputs/random_1k.bin" "1e-4" "5201" "$(max100 edge_inputs/random_1k.bin)" "1000"

echo
echo "=============================="
echo "SUMMARY"
echo "=============================="
column -s, -t check_logs/summary.csv 2>/dev/null || cat check_logs/summary.csv

echo
if grep -q ",FAIL," check_logs/summary.csv; then
  echo "SOME CASES FAILED"
  exit 1
else
  echo "ALL CASES PASSED"
fi
