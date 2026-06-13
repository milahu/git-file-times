#!/bin/sh

# https://serverfault.com/questions/401437
# How to retrieve the last modification date of all files in a Git repository

# print stats every N files
dn=100

function divide() {
  # fixed-point math
  # precision: 3 decimals
  local x=$((1000 * "$1" / "$2"))
  x="${x:0: -3}.${x: -3}"
  if [ "${x:0:1}" = . ]; then
    x="0$x"
  fi
  echo "$x"
}

file_num=0
start=$(date +%s)
last_time=$start

while read -r path; do
  : $((file_num++))
  git --no-pager log -1 --format="%ct $path" "$path"
  ((file_num % dn > 0)) && continue
  now=$(date +%s)
  dt=$((now - last_time))
  last_time=$now
  [ $dt = 0 ] && continue # avoid division by zero
  v=$(divide $dn $dt)
  t=$((now - start))
  avg_v=$(divide $file_num $t)
  echo "stats: files=$file_num time=$t files/s=$v avg_files/s=$avg_v" >&2
done < <(
  git ls-files
)
