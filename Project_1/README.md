# Project 1: Investigating and Reverse Engineering an ELF Executable

`program.c` is a bank transaction system: `main()` plus `create_transactions()`, `process_transactions()` and `print_summary()`, a `Transaction` struct, globals `account_balance` and `rejected_count`, `calloc()`/`free()` for 6 records, a loop and if/else for deposits and withdrawals.

Build: `gcc -Wall -O0 -fno-inline -o program program.c && strip program` and `gcc -Wall -O0 -g -fno-inline -o program_debug program.c`.

## Part B: Static Analysis (readelf, objdump)

- **Header** (`readelf -h program`): `Class: ELF64`, `Machine: Advanced Micro Devices X86-64`, `Type: DYN (Position-Independent Executable file)`, `Entry point address: 0x1100`.
- **Linking:** dynamically linked. `readelf -l` shows `[Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]` and `readelf -d` shows `(NEEDED) Shared library: [libc.so.6]`.

**Sections** (`readelf -S program`):

| Section | Address | Purpose |
|---|---|---|
| .text | 0x1100 | Executable code (`_start`, `main`, the 3 functions) |
| .data | 0x4000 | Initialised globals (`account_balance` at 0x4010) |
| .bss | 0x4020 | Uninitialised globals (`rejected_count` at 0x402c), takes no file space (NOBITS) |
| .plt / .plt.sec | 0x1020 / 0x10a0 | Stubs that jump to library functions |
| .got | 0x3f90 | Table of addresses for library functions, filled at run time |

**Function reconstruction** (`objdump -d -M intel program`; symbols are stripped, so functions are found by `call` targets):

| Address | Function | Evidence |
|---|---|---|
| 0x11e9 | `main()` | `_start` passes it to `__libc_start_main` (`lea rdi,[rip+0xca] # 11e9`); calls 0x12ac, tests result for NULL (error via `fwrite`), calls 0x1415 with `esi=0x6`, calls 0x1624, then `free@plt` |
| 0x12ac | `create_transactions()` | Loads 6 doubles from .rodata, stores types 0/1, `calloc@plt` with `edi=0x6, esi=0x18` (6 × 24 byte struct), loop sets `id = i + 0x3e9` (1001) |
| 0x1415 | `process_transactions()` | Prints header (`printf`, `puts`), loops `i < count`, adds or subtracts the global at 0x4010, increments 0x402c on rejection |
| 0x1624 | `print_summary()` | `puts` then 5 `printf` calls using the totals and both globals |

**Conditional branch** (deposit or withdrawal, in 0x1415):
```
14a3: mov    eax,DWORD PTR [rax+0x10]   ; t->type (offset 16 in struct)
14a6: test   eax,eax                    ; DEPOSIT == 0 ?
14a8: jne    1528                       ; not deposit -> withdrawal path
1541: movsd  xmm0,QWORD PTR [rip+0x2ac7] ; account_balance (0x4010)
1549: comisd xmm0,xmm1                  ; balance vs amount
154d: jb     15ca                       ; amount > balance -> rejected_count++
```

**Loop** (in 0x12ac, `for (i = 0; i < 6; i++)`):
```
1363: mov    DWORD PTR [rbp-0x6c],0x0   ; i = 0
136a: jmp    13f1                       ; jump to the condition
13ed: add    DWORD PTR [rbp-0x6c],0x1   ; i++
13f1: cmp    DWORD PTR [rbp-0x6c],0x5
13f5: jle    136f                       ; repeat while i <= 5
```

**Dynamic linking of library calls:** code calls a PLT stub (`145e: call 10d0 <printf@plt>`), which jumps through a GOT entry (`10d4: jmp QWORD PTR [rip+0x2ee6] # 3fc0`). `objdump -R` shows `0000000000003fc0 R_X86_64_JUMP_SLOT printf@GLIBC_2.2.5` (also `calloc` at 0x3fc8, `free` at 0x3fa8). In `objdump -s -j .got` the entry at 0x3fc0 holds `0x1060`, which points back into `.plt`, so the first call goes to the dynamic linker, which finds `printf` in libc.so.6 and writes its real address into the GOT.

## Part C: Dynamic Analysis (strace ./program)

```
execve("./program", ["./program"], 0x7ffe7bf13e00 /* 122 vars */) = 0
openat(AT_FDCWD, "/usr/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
mmap(0x7377df428000, 1671168, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, 3, 0x28000) = 0x7377df428000
brk(0x64ecee65b000)                     = 0x64ecee65b000
write(1, "ID     TYPE               AMOUNT"..., 48) = 48
exit_group(0)                           = ?
```

| Category | System calls |
|---|---|
| Program startup | `execve`, `access`, `openat`, `read`, `pread64`, `fstat`, `close`, `arch_prctl`, `set_tid_address`, `set_robust_list`, `rseq`, `prlimit64`, `getrandom` |
| Memory management | `mmap`, `mprotect`, `munmap`, `brk` |
| Input/output | `fstat(1, ...)`, `write(1, ...)` |
| Program termination | `exit_group(0)` |

- **malloc/calloc:** `calloc()` does not call the kernel for every request. The heap is grown once with `brk(0x64ecee65b000)` and the 144 bytes come from that space.
- **printf:** `printf()` buffers text in user space. On a terminal stdout is line buffered, so each line becomes one `write(1, ...)` (14 writes). When output is piped it is one `write(1, ..., 604)`.
- **Termination:** returning from `main()` calls `exit()`, which ends with `exit_group(0)`.

## Part D: Debugging (gdb ./program_debug)

```
(gdb) break _start / break main / break process_transactions
Breakpoint 1.1, 0x0000555555555100 in _start ()
(gdb) print account_balance   -> $1 = 1000
(gdb) print &account_balance  -> (double *) 0x555555558010 <account_balance>
Breakpoint 3, process_transactions (txns=0x555555559010, count=6, total_deposits=0x7fffffffce00, total_withdrawals=0x7fffffffce08) at program.c:72
(gdb) backtrace
#0  process_transactions (txns=0x555555559010, count=6, ...) at program.c:72
#1  0x0000555555555266 in main () at program.c:37
(gdb) print *txns@count
$7 = {{id = 1001, amount = 500, type = DEPOSIT}, {id = 1002, amount = 200, type = WITHDRAWAL}, ... {id = 1006, amount = 120.5, type = DEPOSIT}}
(gdb) print i  -> $10 = 0        (gdb) print &i  -> (int *) 0x7fffffffcde4
(gdb) print *t -> $13 = {id = 1001, amount = 500, type = DEPOSIT}
```
`_start` matched twice: 1.2 in `ld-linux-x86-64.so.2` (the dynamic linker runs first) and 1.1 in the program at offset 0x1100.

| Memory | Example | Address | Lifetime |
|---|---|---|---|
| Global (.data/.bss) | `account_balance`, `rejected_count` | 0x555555558010, 0x55555555802c | Whole program; fixed address in the executable's rw segment |
| Heap | 6 `Transaction` records from `calloc` | 0x555555559010 (`[heap]` region) | Until `free(txns)` |
| Stack | `i`, `total_deposits`, `total_withdrawals` | 0x7fffffffcde4, 0x7fffffffce00 | Only while the function runs; `main` passes stack addresses into `process_transactions` |
