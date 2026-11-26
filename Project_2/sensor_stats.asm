BITS 64
DEFAULT REL

SYS_READ    equ 0
SYS_WRITE   equ 1
SYS_OPEN    equ 2
SYS_CLOSE   equ 3
SYS_EXIT    equ 60

STDOUT      equ 1
STDERR      equ 2
O_RDONLY    equ 0

%ifndef BUF_SIZE
  %define BUF_SIZE 4096
%endif

MAX_VALID   equ 100

LF          equ 10
CR          equ 13
TAB         equ 9
SPACE       equ 32

PH_START    equ 0
PH_SIGN     equ 1
PH_DIGITS   equ 2
PH_TRAIL    equ 3

F_INVALID   equ 1
F_NEGATIVE  equ 2
F_CR        equ 4
F_NONEMPTY  equ 8

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

min_val         dq 0x7FFFFFFFFFFFFFFF
max_val         dq -1

section .bss
buffer          resb BUF_SIZE
numbuf          resb 32
path_ptr        resq 1
total_count     resq 1
valid_count     resq 1
invalid_count   resq 1
sum_valid       resq 1

section .text
global _start

_start:
    mov     rax, [rsp]
    lea     rsi, [default_path]
    cmp     rax, 2
    jb      .have_path
    mov     rsi, [rsp + 16]
.have_path:
    mov     [path_ptr], rsi

    mov     eax, SYS_OPEN
    mov     rdi, rsi
    mov     esi, O_RDONLY
    xor     edx, edx
    syscall
    test    rax, rax
    js      open_failed
    mov     r12, rax

    xor     r15d, r15d
    mov     ebx, PH_START
    xor     ebp, ebp

read_loop:
    mov     eax, SYS_READ
    mov     rdi, r12
    lea     rsi, [buffer]
    mov     edx, BUF_SIZE
    syscall
    test    rax, rax
    js      read_failed
    jz      end_of_file

    lea     r13, [buffer]
    mov     r14, rax

char_loop:
    movzx   eax, byte [r13]

    cmp     al, LF
    jne     .not_lf
    call    end_record
    jmp     next_char
.not_lf:
    or      ebp, F_NONEMPTY

    test    ebp, F_CR
    jz      .no_pending_cr
    or      ebp, F_INVALID
    and     ebp, ~F_CR
.no_pending_cr:
    cmp     al, CR
    jne     .not_cr
    or      ebp, F_CR
    jmp     next_char
.not_cr:

    test    ebp, F_INVALID
    jnz     next_char

    cmp     ebx, PH_START
    je      .phase_start
    cmp     ebx, PH_SIGN
    je      .phase_sign
    cmp     ebx, PH_DIGITS
    je      .phase_digits

    cmp     al, SPACE
    je      next_char
    cmp     al, TAB
    je      next_char
    jmp     mark_invalid

.phase_start:
    cmp     al, SPACE
    je      next_char
    cmp     al, TAB
    je      next_char
    cmp     al, '+'
    je      .got_plus
    cmp     al, '-'
    je      .got_minus
    jmp     .phase_sign

.got_minus:
    or      ebp, F_NEGATIVE
.got_plus:
    mov     ebx, PH_SIGN
    jmp     next_char

.phase_sign:
    cmp     al, '0'
    jb      mark_invalid
    cmp     al, '9'
    ja      mark_invalid
    mov     ebx, PH_DIGITS
    jmp     accumulate

.phase_digits:
    cmp     al, SPACE
    je      .to_trail
    cmp     al, TAB
    je      .to_trail
    cmp     al, '0'
    jb      mark_invalid
    cmp     al, '9'
    ja      mark_invalid
    jmp     accumulate

.to_trail:
    mov     ebx, PH_TRAIL
    jmp     next_char

accumulate:
    sub     eax, '0'
    imul    r15, r15, 10
    add     r15, rax
    cmp     r15, MAX_VALID
    jbe     next_char
    mov     r15d, MAX_VALID + 1
    jmp     next_char

mark_invalid:
    or      ebp, F_INVALID

next_char:
    inc     r13
    dec     r14
    jnz     char_loop
    jmp     read_loop

end_of_file:
    test    ebp, F_NONEMPTY
    jz      close_file
    call    end_record

close_file:
    mov     eax, SYS_CLOSE
    mov     rdi, r12
    syscall
    test    rax, rax
    js      close_failed

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

    cmp     qword [valid_count], 0
    je      .no_valid

    lea     rsi, [msg_min]
    mov     rax, [min_val]
    call    print_stat

    lea     rsi, [msg_max]
    mov     rax, [max_val]
    call    print_stat
    jmp     .done

.no_valid:
    lea     rsi, [msg_min]
    call    print_na
    lea     rsi, [msg_max]
    call    print_na

.done:
    xor     edi, edi
    jmp     exit_program

end_record:
    inc     qword [total_count]

    test    ebp, F_INVALID
    jnz     .invalid
    cmp     ebx, PH_DIGITS
    jb      .invalid
    cmp     r15, MAX_VALID
    ja      .invalid
    test    ebp, F_NEGATIVE
    jz      .valid
    test    r15, r15
    jnz     .invalid

.valid:
    inc     qword [valid_count]
    add     [sum_valid], r15
    cmp     r15, [min_val]
    jge     .check_max
    mov     [min_val], r15
.check_max:
    cmp     r15, [max_val]
    jle     .reset
    mov     [max_val], r15
    jmp     .reset

.invalid:
    inc     qword [invalid_count]

.reset:
    xor     r15d, r15d
    mov     ebx, PH_START
    xor     ebp, ebp
    ret

open_failed:
    neg     rax
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
    mov     eax, SYS_CLOSE
    mov     rdi, r12
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

exit_program:
    mov     eax, SYS_EXIT
    syscall

print_stat:
    push    rax
    mov     edi, STDOUT
    call    write_str
    pop     rax
    call    write_uint
    lea     rsi, [newline]
    jmp     write_str

print_na:
    mov     edi, STDOUT
    call    write_str
    lea     rsi, [msg_na]
    call    write_str
    lea     rsi, [newline]
    jmp     write_str

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

write_str:
    xor     edx, edx
.count:
    cmp     byte [rsi + rdx], 0
    je      .write
    inc     rdx
    jmp     .count
.write:
    mov     eax, SYS_WRITE
    syscall
    ret

write_uint:
    lea     rsi, [numbuf + 31]
    mov     byte [rsi], 0
    mov     r8, 10
.next_digit:
    xor     edx, edx
    div     r8
    add     dl, '0'
    dec     rsi
    mov     [rsi], dl
    test    rax, rax
    jnz     .next_digit
    jmp     write_str