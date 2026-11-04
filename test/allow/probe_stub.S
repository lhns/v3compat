/* Loads a seed state, runs the code page, captures every register. No stack use in between. */
#define G(n) (p_regs + 8 * n)(%rip)
#define S(n) (p_seed_regs + 8 * n)(%rip)
.text
.globl run_slot, p_capture, p_recover
.type run_slot, @function
run_slot:
    push %rbx; push %rbp; push %r12; push %r13; push %r14; push %r15
    mov %rsp, p_host_rsp(%rip)
    mov $7, %eax; xor %edx, %edx
    mov p_seed_xs(%rip), %rcx
    xrstor (%rcx)
    pushq p_seed_flags(%rip); popfq
    mov S(0), %rax; mov S(1), %rcx; mov S(2), %rdx; mov S(3), %rbx
    mov S(5), %rbp; mov S(6), %rsi; mov S(7), %rdi; mov S(8), %r8
    mov S(9), %r9; mov S(10), %r10; mov S(11), %r11; mov S(12), %r12
    mov S(13), %r13; mov S(14), %r14; mov S(15), %r15; mov S(4), %rsp
    jmp *p_code(%rip)
p_capture:
    mov %rax, G(0); mov %rcx, G(1); mov %rdx, G(2); mov %rbx, G(3)
    mov %rsp, G(4); mov %rbp, G(5); mov %rsi, G(6); mov %rdi, G(7)
    mov %r8, G(8); mov %r9, G(9); mov %r10, G(10); mov %r11, G(11)
    mov %r12, G(12); mov %r13, G(13); mov %r14, G(14); mov %r15, G(15)
    lea p_priv_top(%rip), %rsp
    pushfq; popq p_flags(%rip); cld
    mov $7, %eax; xor %edx, %edx
    lea p_xs(%rip), %rcx
    xsave (%rcx)
p_recover:   /* a fault's handler resumes here */
    mov p_host_rsp(%rip), %rsp
    cld
    mov $7, %eax; xor %edx, %edx
    mov p_host_xs(%rip), %rcx
    xrstor (%rcx)
    pop %r15; pop %r14; pop %r13; pop %r12; pop %rbp; pop %rbx
    ret
.bss
.balign 4096
    .space 4096
p_priv_top:
.section .note.GNU-stack,"",@progbits
