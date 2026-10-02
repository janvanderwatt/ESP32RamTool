# Taking the UART away from the ROM

**Handing the console over needs two separate moves, and either one on its own
still loses bytes: clear the peripheral's interrupt enable *and* mask the ISR
level.**

## What happens

The ROM leaves `uart_rx_intr_handler` attached to UART0, with the UART's own
interrupt-enable register set. The moment your payload starts polling the FIFO,
the ROM's ISR also drains it, and characters go missing from your console.

Masking the CPU interrupt level stops the handler being *scheduled*, but the
handler is still attached and the peripheral is still asserting. Clearing
`INT_ENA` and `INT_CLR` at the peripheral is what actually makes the FIFO yours.

With only the level mask, the console dropped bytes. With both, it is solid.
This is in `start.S`.

```c
*(volatile uint32_t *)UART_REG_INT_CLR = 0xffffffffu;  /* clear pending */
*(volatile uint32_t *)UART_REG_INT_ENA = 0;             /* stop it asserting */
ets_isr_mask(ETS_MAX_INTR_FLAG);                        /* and stop it running */
```

## `uart_rx_one_char` returns 0 on success

The name suggests the opposite, and it reads through the ROM's own `UartDev`
descriptor rather than the hardware port — so it is not even reading the FIFO you
think it is.

Polling the status register's low byte is unambiguous instead: that field is the
receive-FIFO count.

```c
if ((*(volatile uint32_t *)UART_REG_STATUS & 0xffu) == 0u) {
    continue;                    /* nothing waiting */
}
uint8_t c = (uint8_t)(*(volatile uint32_t *)UART_REG_FIFO & 0xffu);
```

`_start` has already detached the ROM from this UART, so there is no second reader
to race with.

## `waiti` is a trap at an idle prompt

`waiti` looks like the obvious way to idle while waiting for input. Do not use it
here.

`waiti` parks the core until an interrupt arrives. Startup deliberately cleared
the UART interrupt enable, so there is **no interrupt source left**, and `waiti`
never returns. The console dies the moment it goes idle — which is to say,
immediately, at the first prompt.

Poll instead. The cost is a spinning core while you sit at the prompt, which is
the right trade for a tool with nothing else to do.

## Flush before you reset

`reboot` lets the line drain before the part goes away, or the message is lost in
the reset. `uart_tx_flush` waits on the FIFO but **not** on the shift register,
so it is not sufficient on its own:

```c
esp_rom_uart_flush_tx(0);
esp_rom_delay_us(20000);   /* let the last characters actually shift out */
```

The reset then pulls `IO0` low so it lands in the serial bootloader, which means
the next run needs no keypress.

## Do not start a reader thread early

esptool's stub *does* answer `MEM_END` with a four-byte SLIP reply. A console
reader started before the launch completes races esptool for those same bytes and
drops characters from the banner, with no error reported anywhere.

Launch first, then attach the reader.