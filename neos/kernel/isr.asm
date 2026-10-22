bits 64
extern isr_dispatch
global isr_common
global syscall_entry
extern syscall_kstack
global isr_table

section .text
; One stub per vector. CPU pushes an error code only for some exceptions;
; the others get a dummy 0 so struct frame has a uniform layout.
%assign i 0
%rep 256
isr_ %+ i:
%if !(i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30)
    push qword 0
%endif
    push qword i
    jmp isr_common
%assign i i+1
%endrep

; SYSCALL entry (IF masked by SFMASK): build the same frame an interrupt would, then share the common path.
syscall_entry:
    mov [rel user_rsp_tmp], rsp
    mov rsp, [rel syscall_kstack]
    push qword 0x1B             ; ss
    push qword [rel user_rsp_tmp]
    push r11                    ; rflags
    push qword 0x23             ; cs
    push rcx                    ; rip
    push qword 0                ; err
    push qword 0x100            ; vec: Linux syscall
    jmp isr_common

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    cld
    call isr_dispatch       ; returns the frame to resume (may be another thread's)
    mov rsp, rax
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

section .rodata
align 8
isr_table:
%assign i 0
%rep 256
    dq isr_ %+ i
%assign i i+1
%endrep

section .bss
user_rsp_tmp: resq 1
