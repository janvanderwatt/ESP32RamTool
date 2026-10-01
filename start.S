/* RamTool entry point.
 *
 * How control reaches us: the host calls MEM_END with our entry address. The
 * ROM's MemDwnLdStopReqMsgProc stores that address into user_code_start
 * (0x3ffe0400) and main later does `callx8` on it. So we are *called*: there
 * is a live return address and register window above us, and a `ret` from here
 * would drop us back into the ROM's flash-boot path. Everything below therefore
 * installs its own stack and never returns.
 *
 * `callx8` is the Xtensa long call: the target lives in a register, so it
 * reaches anywhere in the 32-bit space. Plain `call` cannot, and our code at
 * 0x40080000 is far outside the range of the ROM functions at 0x4000xxxx.
 *
 * The literal pool comes first because this assembler resolves `l32r` targets
 * only backwards. That is fine: the pool is data, the CPU does not fall into it.
 */

    .section .iram1.entry, "ax"
    .align 4

L_cache_flush:
    .word   0x40009a14        /* Cache_Flush_rom */
L_ets_isr_mask:
    .word   0x400067fc        /* ets_isr_mask */
L_uart_int_ena:
    .word   0x3FF4000C        /* UART0 INT_ENA */
L_uart_int_clr:
    .word   0x3FF40010        /* UART0 INT_CLR */
L_stack_top:
    .word   __stack_top
L_bss_start:
    .word   __bss_start
L_bss_end:
    .word   __bss_end
L_main:
    .word   ramtool_main

    .align 4
    .global ramtool_entry
    .type ramtool_entry, @function
ramtool_entry:
    /* Invalidate the I-cache. The first fetch after this refills from memory,
     * which is where the host just wrote our code. ROM text is mapped and hot,
     * so calling a ROM function before touching our own bytes is safe. */
    l32r    a2, L_cache_flush
    callx8  a2

    /* Our own stack, in the DRAM window the linker script reserved. a1 is sp. */
    l32r    a2, L_stack_top
    mov     a1, a2

    /* Zero .bss. It is NOLOAD, so it is not in the downloaded image and the
     * ROM does not clear it for us. */
    l32r    a2, L_bss_start
    l32r    a3, L_bss_end
    movi    a4, 0
1:  bgeu    a2, a3, 2f
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       1b

    /* Take UART0 away from the ROM.
     *
     * Both steps are needed, and neither alone is sufficient. Masking the CPU
     * interrupt level stops the ROM's uart_rx_intr_handler being scheduled, but
     * the UART's own interrupt-enable register is still set, and the handler
     * had already been attached; the level mask alone left the RX path racy and
     * console bytes went missing. Clearing INT_ENA and INT_CLR at the
     * peripheral is what actually makes the FIFO ours to poll.
     */
2:  l32r    a2, L_uart_int_ena
    movi    a3, 0
    s32i    a3, a2, 0
    l32r    a2, L_uart_int_clr
    movi    a3, 0x0f
    s32i    a3, a2, 0
    l32r    a2, L_ets_isr_mask
    movi    a3, 0x3f
    callx8  a2

    /* Never returns. If it somehow did, spin rather than fall through into the
     * ROM loader's stack frame. */
    l32r    a2, L_main
    callx8  a2
3:  j       3b
    .size ramtool_entry, . - ramtool_entry
