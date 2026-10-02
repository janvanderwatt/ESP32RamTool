# Driving real hardware from the container

**Docker Desktop on Windows will not pass a COM port into a Linux container. A TCP
bridge on the host works, but it costs you DTR/RTS, and the reset truth table is
board-specific.**

Tested on Windows 11 with Docker Desktop 29.6.1 and a CP210x USB-UART bridge.

## `--device` does not work, in three distinct ways

None of the plausible spellings are accepted:

```
$ docker run --device "\\.\COM7"   ...
docker: \\.\COM7 is not an absolute path

$ docker run --device "//./COM7"   ...
docker: error gathering device information while adding custom device "/COM7":
       no such file or directory

$ docker run --device "COM7"   ...
docker: COM7 is not an absolute path
```

The reason is structural: `--device` is a Linux-container feature that maps a path
*inside* the container's own namespace, and a Windows `COMx` is not a path in that
namespace. Docker Desktop runs Linux containers in a VM, and the VM's `/dev` has no
idea the host has a COM port.

## What does work: a TCP bridge

A container can open a TCP socket to the host, and pyserial speaks `socket://`
URLs. esptool hands its port string straight to `serial.serial_for_url`, so no
change to `run.py` is needed at all — this is a property of esptool, not a patch.

```sh
# host
python tools/serial_bridge.py --port COM7 --listen 5000

# container
docker run --rm --add-host host.docker.internal:host-gateway -it \
    ramtool python3 run.py --port socket://host.docker.internal:5000
```

`--add-host host.docker.internal:host-gateway` is only needed for a plain Docker
Engine; Docker Desktop provides `host.docker.internal` already.

Verified end to end: stub upload, payload launch, `part`, `ls`, `cat`, and a full
write cycle (`rw on`, `put`, `add`, `cat`, `rm`, `rw off`) all through the
container.

## The cost: no DTR/RTS, so no reset

DTR and RTS are modem control lines, not data. They cannot cross a TCP socket —
setting them on the far end sets a local Python attribute and does nothing to the
hardware. esptool detects this and says so:

```
NOTE: It's not possible to reset the chip over a TCP socket.
Automatic resetting to bootloader has been disabled, reset the chip manually.
```

Internally it downgrades `connect("default_reset")` to `connect("no_reset")`. So
the bridge has to own the chip reset, which is why `tools/serial_bridge.py` does
it on the client's behalf.

## The cost: the reset truth table is not standard

This is the part that is genuinely board-dependent, and worth getting right rather
than assuming.

The usual ESP32 auto-reset circuit is two transistors. The host holds IO0 low by
asserting DTR, holds the chip in reset by asserting RTS, then releases both in the
right order. That is esptool's `ClassicReset`, and it assumes each line can be
*held* in a state:

```python
port.dtr = False        # IO0 HIGH
port.rts = True         # EN LOW, chip held in reset
time.sleep(0.1)
port.dtr = True         # IO0 LOW
port.rts = False        # EN HIGH, out of reset
time.sleep(reset_delay)
port.dtr = False        # IO0 HIGH again
```

Cheaper boards frequently use a one-transistor variant where DTR **pulses** IO0
and lets it float high again by itself. There is no held state to rely on, and the
sequence has to be timed rather than asserted. esptool half-admits this — its
`UnixTightReset` ends with:

```python
self._setDTR(False)  # Needed in some environments to ensure IO0=HIGH
```

`tools/serial_bridge.py` offers both:

| `--reset` | what it does |
|---|---|
| `classic` (default) | esptool's `ClassicReset`, as above |
| `tight` | esptool's `UnixTightReset`, with the simultaneity approximated |

If the chip will not come up in the bootloader and esptool reports *"No serial
data received"*, try `--reset tight`. That is the symptom of a reset that did not
happen, not of a broken bridge.

**On Windows `tight` is a best effort.** Upstream `UnixTightReset` changes DTR and
RTS in a single termios ioctl. Windows pyserial goes through `SetCommState`, which
is unavoidably two calls, so the two lines cannot be changed atomically. The
ordering is preserved; the simultaneity is not.

Note the polarity inversion: **`DTR True` means IO0 LOW**, because the line is
inverted through the transistor. Getting that backwards does not fail loudly — the
chip resets into its application instead of the bootloader, and esptool reports
only that no serial data arrived. This cost one debugging round here.

## The cost: no flow control, and a 128-byte FIFO

Send commands faster than the payload can consume them and they are lost, silently.

The console polls the UART receive FIFO, which on this part is 128 bytes. `ls` and
`part` spend real time doing SPI reads, during which the payload is not draining
the FIFO. Piping a screenful of commands at once overruns it:

```
> ls
  [f] ECo.csv             69
> rw off
writing locked
> sv                 <-- tail of a command, mangled
unknown. 'help'
```

Pacing the same commands a few seconds apart over the identical bridge produces
clean output. This is not a container problem: it is a property of the design, and
it applies equally to a direct host connection. There is no flow control between
`run.py` and the payload, and at 115200 baud roughly 11 KB/s arrives far faster
than a filesystem listing can be serviced.

Interactive use is unaffected, because a person types at human speed.

## WSL, and why it was not used

WSL2 with `usbipd-win` is the clean answer, and it was available here: WSL2 Ubuntu
was running, but `usbipd-win` was not installed. With it, a CP210x is bound and
attached so it appears as `/dev/ttyUSB0` inside the distro. Then esptool drives the
real DTR/RTS lines, the reset is native, and no proxy sits in the data path.

The route taken instead was to avoid changing the machine: installing `usbipd-win`
needs admin, installs a driver, and takes the device away from Windows while it is
attached, so your Windows tools lose COM7 for as long as it is bound.

If you want the container talking to hardware with no caveats, this is the way:

```sh
# once, elevated
winget install dorssel.usbipd-win

# bind and attach (Windows sees the device disappear)
usbipd list
usbipd bind --busid <BUSID>
usbipd attach --wsl --busid <BUSID>

# inside WSL
ls /dev/ttyUSB*
sudo apt install -y python3-pip esptool
python3 run.py --port /dev/ttyUSB0
```

To give the device back to Windows: `usbipd unbind --busid <BUSID>`.

## Summary

| approach | reset | caveats |
|---|---|---|
| host, direct COM port | native, correct | none |
| container via TCP bridge | bridge-owned, `--reset` selectable | no flow control; `tight` is approximate on Windows |
| WSL + `usbipd-win` | native, correct | needs install, and reclaims the device from Windows |

For CI that never touches hardware — which is the usual reason to want a container
— none of this applies. The container builds and checks the artifacts; the runtime
claims belong to a real chip, and a real chip was used for them.