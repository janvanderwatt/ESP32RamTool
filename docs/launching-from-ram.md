# The ROM loader never calls your entry point

**`MEM_END` with a non-zero entry records the address and returns to the command
loop. The esptool stub is not a convenience here — it is the only thing that
actually jumps to your code.**

## What happens

The obvious way to run code from RAM is to skip esptool entirely: talk to the
ROM serial bootloader directly, then `MEM_BEGIN` / `MEM_DATA` / `MEM_END` with
the address you want to start at. This does not work, and it fails quietly.

`MEM_END` with a non-zero entry does not call the entry. Looking at the ROM's
`MemDwnLdStopReqMsgMsgProc`, it only stores the address into the `user_code_start`
word at `0x3FFE0400` and returns into the command loop.

On a normal boot that word *is* used: `main` reads it and `callx8`es it. But the
download-mode path never reaches that code. The address is written down and then
ignored. The chip sits in the bootloader answering nothing, which from the host
looks exactly like a crash.

## What to do

Use esptool's flasher stub as the launch mechanism. `run_stub()` uploads a small
loader to IRAM `0x400BE000`, and the stub's own `MEM_END` handler jumps to the
entry you hand it. It is still a RAM-only upload, so nothing is written to flash.

Driving it means using the esptool **Python API**, not its CLI:

- The CLI resets the chip when it finishes. That is the default
  (`--after hard_reset`) and it is right for flashing and wrong for this, because
  the reset undoes the entire point.
- `--after no_reset_stub` is better but still leaves the stub resident and still
  needs a second command to actually run the code.
- Never call `hard_reset()` or `soft_reset()` from the API either.

## One more thing that bites on the way out

Deassert **DTR and RTS before closing the serial port**. On the usual CP210x
auto-reset wiring, RTS low pulls `EN` low. Close the port with RTS asserted and
the part resets behind your back, after your program has already reported
success.

```python
port.dtr = False
port.rts = False
port.close()
```

## Related

- [uart-handover.md](uart-handover.md) — taking the console away from the ROM once
  the stub has launched you.
- [rom-symbols.md](rom-symbols.md) — `SPIRead` and friends, which the stub path
  also depends on.