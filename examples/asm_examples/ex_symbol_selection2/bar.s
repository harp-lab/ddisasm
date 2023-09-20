    .section .rodata
.LC0_bar:
    .string "bar: %d\n"

    .section .text

.weak  fun
.type  fun, @function
fun:
    pushq   %rbp
    movq    %rsp, %rbp
    movq    $7, %rsi
    leaq    .LC0_bar(%rip), %rdi
    movq    $0, %rax
    call    printf@PLT
    movq    $0, %rax
    popq    %rbp
    ret

    .align 16

.globl bar_fun
.type  bar_fun, @function
bar_fun:
    pushq   %rbp
    movq    %rsp, %rbp
    call    fun@PLT
    movq    $0, %rax
    popq    %rbp
    ret
