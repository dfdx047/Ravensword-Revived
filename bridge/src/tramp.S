// One entry per native method registered by the guest. Each loads its slot and jumps to the
// common code, which saves the arm64 argument registers and calls bridge_native_dispatch.
    .text
    .globl bridge_tramp_table
    .type bridge_tramp_table, %function
    .p2align 3
bridge_tramp_table:
    .set slot, 0
    .rept 512
    mov x16, #slot
    b bridge_tramp_common
    .set slot, slot + 1
    .endr

    .type bridge_tramp_common, %function
bridge_tramp_common:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #128
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp d0, d1, [sp, #64]
    stp d2, d3, [sp, #80]
    stp d4, d5, [sp, #96]
    stp d6, d7, [sp, #112]
    mov w0, w16
    mov x1, sp
    add x2, sp, #64
    add x3, sp, #144
    bl bridge_native_dispatch
    fmov d0, x0
    add sp, sp, #128
    ldp x29, x30, [sp], #16
    ret
    .section .note.GNU-stack,"",%progbits
