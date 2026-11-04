# Freestanding (no libc, whose own AVX2 string code would be counted too), so the
# per-class totals of the client are exact. SIGILLs on a CPU without AVX2, which is fine:
# the block is counted when it is translated, before it runs.
# avx2: 3 (vpaddd ymm, vpbroadcastd, vpermq)  fma: 1  bmi: 4 (andn blsr shlx pext)
# lzcnt: 1  tzcnt: 1  movbe: 1
        .text
        .globl  _start
_start:
        lea     -8(%rsp), %rsp
        vpaddd  %ymm1, %ymm2, %ymm3
        vpbroadcastd %xmm1, %ymm4
        vpermq  $0x1b, %ymm4, %ymm5
        vfmadd231ps %ymm1, %ymm2, %ymm6
        andn    %esi, %edi, %eax
        blsr    %eax, %edx
        shlx    %eax, %edx, %ecx
        pext    %eax, %edx, %ecx
        lzcnt   %edi, %eax
        tzcnt   %esi, %eax
        movbe   (%rsp), %eax
        vzeroupper
        mov     $60, %eax
        xor     %edi, %edi
        syscall
        .section .note.GNU-stack,"",@progbits
