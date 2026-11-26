;==============================================================================
; sensor_stats.asm - Sensor reading validator and statistics calculator
;
; Assembler / environment : NASM 2.16, GNU ld 2.42, Linux x86-64 (Ubuntu 24.04)
; Uses Linux system calls directly (no C library).
;
; Build:
;     nasm -f elf64 sensor_stats.asm -o sensor_stats.o
;     ld sensor_stats.o -o sensor_stats
; Run:
;     ./sensor_stats                 (reads sensor_readings.txt)
;     ./sensor_stats other_file.txt  (optional: read another file)
;
; A record is one line of the file. A record is VALID when, after optional
; leading/trailing spaces or tabs, it holds an optional sign followed by
; decimal digits only, and its value is between 0 and 100 inclusive.
; Everything else is INVALID: empty lines, text, mixed text and digits,
; decimals, negative numbers and values above 100.
; Both LF (Unix) and CRLF (Windows) line endings are accepted.
;
; Exit status: 0 = success, 1 = open error, 2 = read error, 3 = close error
;==============================================================================

BITS 64
DEFAULT REL                     ; all memory references are RIP-relative

;------------------------------------------------------------------------------
; Constants
;------------------------------------------------------------------------------
SYS_READ    equ 0               ; Linux x86-64 system call numbers
SYS_WRITE   equ 1
SYS_OPEN    equ 2
SYS_CLOSE   equ 3
SYS_EXIT    equ 60

STDOUT      equ 1
STDERR      equ 2
O_RDONLY    equ 0

%ifndef BUF_SIZE                ; read buffer size; can be overridden with
  %define BUF_SIZE 4096         ; nasm -DBUF_SIZE=n to test chunk boundaries
%endif

MAX_VALID   equ 100             ; valid range is 0..MAX_VALID

LF          equ 10              ; '\n'  end of line
CR          equ 13              ; '\r'  first byte of a Windows CRLF ending
TAB         equ 9
SPACE       equ 32

; Parser phase for the current line (held in ebx). The order matters:
; any phase >= PH_DIGITS means at least one digit has been read.
PH_START    equ 0               ; nothing but spaces seen so far
PH_SIGN     equ 1               ; '+' or '-' seen, a digit must follow
PH_DIGITS   equ 2               ; reading digits
PH_TRAIL    equ 3               ; digits finished, only spaces may follow

; Flags for the current line (bit mask held in ebp)
F_INVALID   equ 1               ; line already known to be invalid
F_NEGATIVE  equ 2               ; a '-' sign was read
F_CR        equ 4               ; last character was CR (expecting LF)
F_NONEMPTY  equ 8               ; line contains at least one character

;------------------------------------------------------------------------------
; Register usage while the file is processed
;   r12  file descriptor
;   r13  pointer to the current character in the buffer
;   r14  number of unprocessed bytes left in the buffer
;   r15  numeric value of the current line
;   ebx  parser phase (PH_*)
;   ebp  line flags (F_*)
; These registers are preserved by the kernel across syscall, so they keep
; their values between successive read calls. (syscall overwrites rax, rcx
; and r11, so those are only used for temporary values.)
;------------------------------------------------------------------------------

section .data
default_path    db "sensor_readings.txt", 0

msg_total       db "Total records: ", 0
msg_valid       db "Valid measurements: ", 0
msg_invalid     db "Invalid measurements: ", 0
msg_sum         db "Sum of valid measurements: ", 0
msg_min         db "Minimum measurement: ", 0
msg_max         db "Maximum measurement: ", 0
msg_na          db "N/A", 0
newline         db LF, 0

err_open        db "Error: cannot open file '", 0
err_read        db "Error: failed while reading file '", 0
err_close       db "Error: failed to close file '", 0
err_errno       db "' (errno ", 0
err_end         db ")", LF, 0

; Statistics stored in memory. min starts above any valid value and max
; below any valid value, so the first valid reading replaces both.
min_val         dq 0x7FFFFFFFFFFFFFFF
max_val         dq -1

section .bss
buffer          resb BUF_SIZE   ; file data is read into this buffer
numbuf          resb 32         ; scratch space for number-to-text conversion
path_ptr        resq 1          ; address of the file name being processed
total_count     resq 1          ; .bss is zero-filled, so all counters
valid_count     resq 1          ; and the sum start at 0
invalid_count   resq 1
sum_valid       resq 1

section .text
global _start

;==============================================================================
; Program entry point
;==============================================================================
_start:
    ;--------------------------------------------------------------------------
    ; Choose the input file. On entry [rsp] = argc and [rsp+16] = argv[1].
    ; Default to sensor_readings.txt unless a file name was given.
    ;--------------------------------------------------------------------------
    mov     rax, [rsp]                  ; argc
    lea     rsi, [default_path]
    cmp     rax, 2
    jb      .have_path                  ; no argument: keep the default
    mov     rsi, [rsp + 16]             ; argv[1]
.have_path:
    mov     [path_ptr], rsi

    ;--------------------------------------------------------------------------
    ; FILE OPEN: open(path, O_RDONLY). On success rax is the new file
    ; descriptor; on failure rax is -errno (a negative number).
    ;--------------------------------------------------------------------------
    mov     eax, SYS_OPEN
    mov     rdi, rsi                    ; arg1: path
    mov     esi, O_RDONLY               ; arg2: flags
    xor     edx, edx                    ; arg3: mode (unused when reading)
    syscall
    test    rax, rax
    js      open_failed                 ; sign bit set -> error
    mov     r12, rax                    ; keep the file descriptor

    ; Reset the per-line parser state before the first line
    xor     r15d, r15d                  ; value = 0
    mov     ebx, PH_START               ; phase = start of line
    xor     ebp, ebp                    ; flags = none

;==============================================================================
; OUTER LOOP - read the file one buffer at a time.
; read() returns the number of bytes read, 0 at end of file, or -errno.
; Lines can be split across two reads; this is handled because the parser
; state (r15, ebx, ebp) is kept in registers between reads.
;==============================================================================
read_loop:
    mov     eax, SYS_READ
    mov     rdi, r12                    ; arg1: file descriptor
    lea     rsi, [buffer]               ; arg2: destination buffer
    mov     edx, BUF_SIZE               ; arg3: maximum bytes to read
    syscall
    test    rax, rax
    js      read_failed                 ; negative -> read error
    jz      end_of_file                 ; zero -> no more data

    lea     r13, [buffer]               ; r13 = first byte of the new data
    mov     r14, rax                    ; r14 = number of bytes to process

;==============================================================================
; INNER LOOP - examine the buffer one character at a time.
; Each character either ends the record (LF) or updates the parser state.
;==============================================================================
char_loop:
    movzx   eax, byte [r13]             ; al = current character (rax zeroed)

    ;--- Line boundary: LF ends the current record ---------------------------
    cmp     al, LF
    jne     .not_lf
    call    end_record                  ; classify the finished line
    jmp     next_char
.not_lf:
    or      ebp, F_NONEMPTY             ; this line has content

    ;--- CRLF handling: a CR is only allowed directly before LF --------------
    test    ebp, F_CR                   ; was the previous character CR?
    jz      .no_pending_cr
    or      ebp, F_INVALID              ; CR followed by something other
    and     ebp, ~F_CR                  ; than LF: stray CR, line invalid
.no_pending_cr:
    cmp     al, CR
    jne     .not_cr
    or      ebp, F_CR                   ; remember the CR; if LF comes next
    jmp     next_char                   ; it is a normal Windows line ending
.not_cr:

    ;--- Once a line is invalid, skip its remaining characters ---------------
    test    ebp, F_INVALID
    jnz     next_char

    ;--- Validation: branch on the parser phase ------------------------------
    cmp     ebx, PH_START
    je      .phase_start
    cmp     ebx, PH_SIGN
    je      .phase_sign
    cmp     ebx, PH_DIGITS
    je      .phase_digits

    ; PH_TRAIL: after the number only spaces/tabs are allowed ("12 3" fails)
    cmp     al, SPACE
    je      next_char
    cmp     al, TAB
    je      next_char
    jmp     mark_invalid

.phase_start:                           ; before the number
    cmp     al, SPACE                   ; skip leading spaces/tabs
    je      next_char
    cmp     al, TAB
    je      next_char
    cmp     al, '+'
    je      .got_plus
    cmp     al, '-'
    je      .got_minus
    jmp     .phase_sign                 ; anything else must be a digit

.got_minus:
    or      ebp, F_NEGATIVE             ; remember the minus sign
.got_plus:
    mov     ebx, PH_SIGN                ; a digit must come next
    jmp     next_char

.phase_sign:                            ; a digit is required here
    cmp     al, '0'
    jb      mark_invalid                ; below '0' -> not a digit
    cmp     al, '9'
    ja      mark_invalid                ; above '9' -> not a digit
    mov     ebx, PH_DIGITS              ; first digit of the number
    jmp     accumulate

.phase_digits:                          ; inside the number
    cmp     al, SPACE                   ; a space ends the number
    je      .to_trail
    cmp     al, TAB
    je      .to_trail
    cmp     al, '0'                     ; otherwise it must be a digit
    jb      mark_invalid                ; ('.', letters, etc. are invalid)
    cmp     al, '9'
    ja      mark_invalid
    jmp     accumulate

.to_trail:
    mov     ebx, PH_TRAIL
    jmp     next_char

;------------------------------------------------------------------------------
; NUMERIC CONVERSION: value = value * 10 + (character - '0')
; Example "42": 0*10 + 4 = 4, then 4*10 + 2 = 42.
; Any value above 100 is already invalid, so it is clamped to 101. This
; keeps the number small and prevents overflow on very long digit strings.
;------------------------------------------------------------------------------
accumulate:
    sub     eax, '0'                    ; ASCII digit -> number 0..9
    imul    r15, r15, 10                ; shift previous digits left
    add     r15, rax                    ; add the new digit
    cmp     r15, MAX_VALID
    jbe     next_char
    mov     r15d, MAX_VALID + 1         ; clamp: out of range
    jmp     next_char

mark_invalid:
    or      ebp, F_INVALID              ; line contains a disallowed character

next_char:
    inc     r13                         ; advance to the next character
    dec     r14                         ; one fewer byte left in the buffer
    jnz     char_loop                   ; more bytes -> continue inner loop
    jmp     read_loop                   ; buffer used up -> read more

;------------------------------------------------------------------------------
; End of file. If the last line had no trailing LF it has not been counted
; yet, so classify it now. (A file ending in LF has no extra record.)
;------------------------------------------------------------------------------
end_of_file:
    test    ebp, F_NONEMPTY
    jz      close_file
    call    end_record

;------------------------------------------------------------------------------
; FILE CLOSE: close(fd) releases the file descriptor.
;------------------------------------------------------------------------------
close_file:
    mov     eax, SYS_CLOSE
    mov     rdi, r12
    syscall
    test    rax, rax
    js      close_failed

;==============================================================================
; OUTPUT - print the statistics in the required format
;==============================================================================
    lea     rsi, [msg_total]
    mov     rax, [total_count]
    call    print_stat

    lea     rsi, [msg_valid]
    mov     rax, [valid_count]
    call    print_stat

    lea     rsi, [msg_invalid]
    mov     rax, [invalid_count]
    call    print_stat

    lea     rsi, [msg_sum]
    mov     rax, [sum_valid]
    call    print_stat

    ; Minimum and maximum only exist if at least one reading was valid
    cmp     qword [valid_count], 0
    je      .no_valid

    lea     rsi, [msg_min]
    mov     rax, [min_val]
    call    print_stat

    lea     rsi, [msg_max]
    mov     rax, [max_val]
    call    print_stat
    jmp     .done

.no_valid:                              ; no valid readings: print N/A
    lea     rsi, [msg_min]
    call    print_na
    lea     rsi, [msg_max]
    call    print_na

.done:
    xor     edi, edi                    ; exit status 0
    jmp     exit_program

;==============================================================================
; end_record - classify the line that just ended and update the statistics,
; then reset the parser for the next line.
;   in : r15 = value, ebx = phase, ebp = flags
;==============================================================================
end_record:
    inc     qword [total_count]         ; every line is a record

    ; Decide whether the record is valid
    test    ebp, F_INVALID              ; bad character seen?
    jnz     .invalid
    cmp     ebx, PH_DIGITS              ; phase below PH_DIGITS means no
    jb      .invalid                    ; digit at all (empty, "-", spaces)
    cmp     r15, MAX_VALID              ; above 100?
    ja      .invalid
    test    ebp, F_NEGATIVE             ; negative numbers are below 0,
    jz      .valid                      ; except "-0", which equals 0
    test    r15, r15
    jnz     .invalid

.valid:
    ; STATISTICS: count, sum, minimum and maximum
    inc     qword [valid_count]
    add     [sum_valid], r15            ; sum += value
    cmp     r15, [min_val]
    jge     .check_max
    mov     [min_val], r15              ; new smallest value
.check_max:
    cmp     r15, [max_val]
    jle     .reset
    mov     [max_val], r15              ; new largest value
    jmp     .reset

.invalid:
    inc     qword [invalid_count]

.reset:                                 ; start a fresh line
    xor     r15d, r15d
    mov     ebx, PH_START
    xor     ebp, ebp
    ret

;==============================================================================
; Error handling. The kernel returns -errno; it is negated and printed
; together with the file name, and the program exits with a non-zero status.
;==============================================================================
open_failed:
    neg     rax                         ; -errno -> errno
    mov     rbx, rax
    lea     rsi, [err_open]
    call    print_error
    mov     edi, 1
    jmp     exit_program

read_failed:
    neg     rax
    mov     rbx, rax
    lea     rsi, [err_read]
    call    print_error
    mov     eax, SYS_CLOSE              ; release the descriptor before
    mov     rdi, r12                    ; exiting
    syscall
    mov     edi, 2
    jmp     exit_program

close_failed:
    neg     rax
    mov     rbx, rax
    lea     rsi, [err_close]
    call    print_error
    mov     edi, 3
    jmp     exit_program

;------------------------------------------------------------------------------
; exit_program - terminate the process.   in: edi = exit status
;------------------------------------------------------------------------------
exit_program:
    mov     eax, SYS_EXIT
    syscall

;==============================================================================
; Output helpers
;==============================================================================

;------------------------------------------------------------------------------
; print_stat - print "<label><value>\n" to stdout
;   in: rsi = label (NUL-terminated), rax = unsigned value
;------------------------------------------------------------------------------
print_stat:
    push    rax                         ; save value across write_str
    mov     edi, STDOUT
    call    write_str                   ; label
    pop     rax
    call    write_uint                  ; value (rdi is still STDOUT)
    lea     rsi, [newline]
    jmp     write_str                   ; tail call: newline, then return

;------------------------------------------------------------------------------
; print_na - print "<label>N/A\n" to stdout.   in: rsi = label
;------------------------------------------------------------------------------
print_na:
    mov     edi, STDOUT
    call    write_str
    lea     rsi, [msg_na]
    call    write_str
    lea     rsi, [newline]
    jmp     write_str

;------------------------------------------------------------------------------
; print_error - print "<prefix><path>' (errno N)\n" to stderr
;   in: rsi = message prefix, rbx = errno
;------------------------------------------------------------------------------
print_error:
    mov     edi, STDERR
    call    write_str
    mov     rsi, [path_ptr]
    call    write_str
    lea     rsi, [err_errno]
    call    write_str
    mov     rax, rbx
    call    write_uint
    lea     rsi, [err_end]
    jmp     write_str

;------------------------------------------------------------------------------
; write_str - write a NUL-terminated string
;   in: rdi = file descriptor, rsi = string
; Loop counts characters up to the NUL byte, then one write() sends them.
;------------------------------------------------------------------------------
write_str:
    xor     edx, edx                    ; rdx = length
.count:
    cmp     byte [rsi + rdx], 0
    je      .write
    inc     rdx
    jmp     .count
.write:
    mov     eax, SYS_WRITE              ; write(fd=rdi, buf=rsi, len=rdx)
    syscall                             ; rdi/rsi/rdx are preserved
    ret

;------------------------------------------------------------------------------
; write_uint - write an unsigned 64-bit integer in decimal
;   in: rdi = file descriptor, rax = value
; Repeated division by 10 gives the digits from right to left; each
; remainder is turned into an ASCII character and stored backwards.
;------------------------------------------------------------------------------
write_uint:
    lea     rsi, [numbuf + 31]          ; end of the scratch buffer
    mov     byte [rsi], 0               ; string terminator
    mov     r8, 10
.next_digit:
    xor     edx, edx                    ; rdx:rax / 10
    div     r8                          ; rax = quotient, rdx = remainder
    add     dl, '0'                     ; remainder 0..9 -> '0'..'9'
    dec     rsi
    mov     [rsi], dl                   ; store digit (right to left)
    test    rax, rax
    jnz     .next_digit                 ; until the quotient is 0
    jmp     write_str                   ; rsi = first digit