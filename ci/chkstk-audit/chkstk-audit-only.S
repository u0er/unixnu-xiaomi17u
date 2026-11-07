/* Audit experiment only. The Nezha runtime stack/guard contract is unverified. */
#if !defined(CHKSTK_AUDIT_ONLY) || !defined(CHKSTK_AUDIT_STRIDE)
#error This unverified experiment must never be used as a production helper.
#endif
#if CHKSTK_AUDIT_STRIDE != 4096
#error Only the explicitly labelled 4 KiB diagnostic experiment is assembled here.
#endif

    .text
    .p2align 2
    .globl ___chkstk_darwin
___chkstk_darwin:
    bti     c
    pacibsp
    mov     w9, w9
    cbz     w9, Lreturn
    mov     x10, sp
Lprobe_loop:
    cmp     w9, #1, lsl #12
    b.lo    Lprobe_tail
    subs    x10, x10, #1, lsl #12
    b.lo    Laddress_underflow
    ldr     x11, [x10]
    sub     w9, w9, #1, lsl #12
    cbnz    w9, Lprobe_loop
    b       Lreturn
Lprobe_tail:
    subs    x10, x10, x9
    b.lo    Laddress_underflow
    ldr     x11, [x10]
Lreturn:
    retab
Laddress_underflow:
    brk     #0xc471
