# Project 2: Assembly-Based Sensor Data Validation and Analysis

`sensor_stats.asm` reads `sensor_readings.txt`, validates each line as an integer from 0 to 100, and prints record counts, sum, minimum and maximum. It uses Linux system calls directly (no C library).

## Environment

- Assembler: NASM 3.01 (`nasm -f elf64`)
- Linker: GNU ld 2.46
- System: Ubuntu 26.04 LTS, Linux x86-64

## Build and Run

```
nasm -f elf64 sensor_stats.asm -o sensor_stats.o
ld sensor_stats.o -o sensor_stats
./sensor_stats
```

## Test Input (`sensor_readings.txt`)

| Line | Content | Result | Line | Content | Result |
|---|---|---|---|---|---|
| 1 | `25` | valid | 12 | `12a` | invalid (non-numeric) |
| 2 | `30` | valid | 13 | `  64  ` | valid (spaces trimmed) |
| 3 | `invalid` | invalid (text) | 14 | `+15` | valid |
| 4 | `18` | valid | 15 | (empty) | invalid (empty) |
| 5 | `42` | valid | 16 | `3.5` | invalid (decimal) |
| 6 | `-5` | invalid (below 0) | 17 | `1 2` | invalid |
| 7 | (empty) | invalid (empty) | 18 | `   ` | invalid (spaces only) |
| 8 | `100` | valid | 19 | `999999999999999999999` | invalid (above 100) |
| 9 | `0` | valid | 20 | `99` | valid |
| 10 | `101` | invalid (above 100) | 21 | `50` | valid |
| 11 | `007` | valid (7) | | | |

## Execution Evidence

```
$ ./sensor_stats
Total records: 21
Valid measurements: 11
Invalid measurements: 10
Sum of valid measurements: 450
Minimum measurement: 0
Maximum measurement: 100
```

Valid values: 25 + 30 + 18 + 42 + 100 + 0 + 7 + 64 + 15 + 99 + 50 = 450.

## How the Requirements Are Met

**File handling**
- `open` (syscall 2) with `O_RDONLY`. A negative return value is an error, which prints `Error: cannot open file ...` and exits with status 1.
- `read` (syscall 0) into a 4096 byte `buffer` in a loop until it returns 0. A read error exits with status 2.
- `close` (syscall 3) after processing. A close error exits with status 3.

**Record extraction**
- `char_loop` loads one byte at a time with `movzx eax, byte [r13]`, then `inc r13` / `dec r14` moves to the next byte.
- LF (10) ends a record and calls `end_record`. A CR (13) directly before LF is skipped, so CRLF files work.
- A line with no digits (empty or spaces only) is counted as invalid. A last line without a trailing LF is still counted.

**Conversion and validation**
- Each digit is added with `sub eax, '0'`, `imul r15, r15, 10`, `add r15, rax`.
- The phase in `ebx` (start, sign, digits, trailing) and the flags in `ebp` reject letters, decimals, inner spaces and negative numbers.
- Values above 100 are capped at 101, so long digit strings cannot overflow.

**Statistics**
- Counters `total_count`, `valid_count`, `invalid_count` and `sum_valid` are in `.bss`.
- `min_val` and `max_val` are updated with `cmp` / `jge` / `jle` in `end_record`.

**Output and termination**
- `write_uint` converts numbers to text by dividing by 10, and `write_str` prints with `write` (syscall 1).
- If there are no valid values, the minimum and maximum are printed as `N/A`.
- The program ends with `exit` (syscall 60), status 0.
