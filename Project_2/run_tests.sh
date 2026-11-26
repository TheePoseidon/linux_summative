#!/usr/bin/env bash
# run_tests.sh - build sensor_stats and run it against several test inputs.
# Usage: ./run_tests.sh        (run from the folder containing the .asm file)

set -u
cd "$(dirname "$0")"

TMP=tests                     # generated test inputs are kept here
rm -rf "$TMP" && mkdir -p "$TMP"

# Print a command, then run it
run() {
    echo "\$ $*"
    "$@"
    local status=$?
    echo "[exit status: $status]"
    echo
}

section() {
    echo "=============================================================="
    echo "$1"
    echo "=============================================================="
}

section "ENVIRONMENT AND BUILD"
run uname -srm
run grep PRETTY_NAME /etc/os-release
run nasm -v
run ld -v
run nasm -f elf64 sensor_stats.asm -o sensor_stats.o
run ld sensor_stats.o -o sensor_stats

section "TEST 1 - Example from the assignment brief"
printf '25\n30\ninvalid\n18\n42\n-5\n' > "$TMP/example.txt"
run cat -A "$TMP/example.txt"
run ./sensor_stats "$TMP/example.txt"

section "TEST 2 - sensor_readings.txt (valid, invalid and empty records)"
run cat -nA sensor_readings.txt
run ./sensor_stats

section "TEST 3 - Same data with Windows CRLF line endings"
sed 's/$/\r/' sensor_readings.txt > "$TMP/crlf.txt"
run cat -nA "$TMP/crlf.txt"
run ./sensor_stats "$TMP/crlf.txt"

section "TEST 4 - Last line has no trailing newline"
printf '10\n20\n30' > "$TMP/no_newline.txt"
echo "\$ cat -A $TMP/no_newline.txt"
cat -A "$TMP/no_newline.txt"
echo "      <- no \$ after 30: the file has no final newline"
echo
run ./sensor_stats "$TMP/no_newline.txt"

section "TEST 5 - No valid measurements"
printf 'abc\n\n-1\n150\n\n' > "$TMP/no_valid.txt"
run cat -A "$TMP/no_valid.txt"
run ./sensor_stats "$TMP/no_valid.txt"

section "TEST 6 - Empty file (0 bytes)"
: > "$TMP/empty.txt"
run wc -c "$TMP/empty.txt"
run ./sensor_stats "$TMP/empty.txt"

section "TEST 7 - File cannot be opened (does not exist)"
run ./sensor_stats "$TMP/missing.txt"

section "TEST 8 - Read error (path is a directory)"
run ./sensor_stats "$TMP"

section "TEST 9 - Records split across reads (1-byte buffer)"
nasm -f elf64 -DBUF_SIZE=1 sensor_stats.asm -o "$TMP/tiny.o" && ld "$TMP/tiny.o" -o "$TMP/sensor_stats_tiny"
echo "Built a copy with BUF_SIZE=1, so every read() returns one byte."
./sensor_stats                  > "$TMP/normal_lf.out"
"$TMP/sensor_stats_tiny"        > "$TMP/tiny_lf.out"
./sensor_stats "$TMP/crlf.txt"  > "$TMP/normal_crlf.out"
"$TMP/sensor_stats_tiny" "$TMP/crlf.txt" > "$TMP/tiny_crlf.out"
run cat "$TMP/tiny_lf.out"
if cmp -s "$TMP/normal_lf.out" "$TMP/tiny_lf.out" && cmp -s "$TMP/normal_crlf.out" "$TMP/tiny_crlf.out"; then
    echo "Result: output identical to the 4096-byte buffer build (LF and CRLF)."
else
    echo "Result: OUTPUT DIFFERS"
fi

section "TEST 10 - System calls: file opened, read, closed, process exits"
if command -v strace > /dev/null; then
    echo "\$ strace -e trace=open,read,close,exit ./sensor_stats tests/example.txt > /dev/null"
    strace -e trace=open,read,close,exit ./sensor_stats tests/example.txt 2>&1 > /dev/null
    echo
else
    echo "strace not installed - skipped"
fi