"""Bridge a host serial port to a TCP socket, so a container can reach it.

Docker Desktop on Windows cannot hand a COM port to a Linux container -- all the
plausible --device spellings fail -- but a container can open a TCP socket to the
host. pyserial understands socket:// URLs and esptool passes its port string
straight to serial.serial_for_url, so that combination works with no changes to
run.py at all.

    # host
    python tools/serial_bridge.py --port COM7 --listen 5000
    # container
    docker run --rm --add-host host.docker.internal:host-gateway -it \
        ramtool python3 run.py --port socket://host.docker.internal:5000

Two things do not survive the trip, and both are worth understanding rather than
working around blindly.

1. DTR/RTS cannot cross a socket. Setting them on the far end is a local Python
   attribute, not a wire signal. esptool notices, downgrades itself to
   connect("no_reset") and says so, which means the bridge owns the chip reset
   instead. That is why --reset exists below.

2. The reset truth table is board-specific. The usual two-transistor auto-reset
   circuit lets the host hold IO0 low, so a plain "assert DTR, release EN"
   sequence works. Cheaper boards often have a one-transistor circuit where DTR
   only *pulses* IO0 and lets it float high again, so the sequence has to be
   timed rather than held. esptool's own UnixTightReset ends with a comment
   admitting this ("Needed in some environments to ensure IO0=HIGH"). Pick with
   --reset if the chip does not come up in the bootloader.
"""
import argparse
import socket
import sys
import threading
import time

try:
    import serial
except ImportError:  # noqa: BLE001
    sys.exit("this needs pyserial: pip install pyserial")


def reset_classic(port, reset_delay=0.1):
    """esptool's ClassicReset, verbatim.

    Copied from esptool/reset.py rather than reasoned about, because the polarity
    is inverted through the auto-reset transistors and getting it backwards does
    not fail loudly: the chip resets into its application instead of the
    bootloader, and esptool then reports only "No serial data received".

        DTR False -> IO0 HIGH
        RTS True  -> EN LOW, held in reset
        DTR True  -> IO0 LOW
        RTS False -> EN HIGH, out of reset
        DTR False -> IO0 HIGH again
    """
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.dtr = True
    port.rts = False
    time.sleep(reset_delay)
    port.dtr = False


def reset_tight(port, reset_delay=0.1):
    """esptool's UnixTightReset, with the simultaneity approximated.

    Upstream changes DTR and RTS in a single termios ioctl. Windows pyserial
    goes through SetCommState, which is unavoidably two calls, so this is a
    best-effort equivalent rather than the real thing. It exists because boards
    that only pulse IO0 can be more sensitive to the ordering than to the
    simultaneity.
    """
    def both(dtr, rts):
        port.dtr = dtr
        port.rts = rts

    both(False, False)
    both(True, True)
    both(False, True)      # IO0 HIGH, EN LOW, held in reset
    time.sleep(0.1)
    both(True, False)       # IO0 LOW, EN HIGH, out of reset
    time.sleep(reset_delay)
    both(False, False)      # IO0 HIGH
    port.dtr = False        # belt and braces, as upstream notes


RESETS = {"classic": reset_classic, "tight": reset_tight}


def sock_to_port(conn, port):
    """Container -> chip. Sockets have recv(), not read()."""
    total = 0
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
            port.write(data)
            total += len(data)
    except OSError as exc:
        print(f"[in] {exc}", flush=True)
    print(f"[in] {total} bytes to chip", flush=True)


def port_to_sock(port, conn):
    """Chip -> container.

    The read timeout is set, so an empty result means "nothing waiting", not end
    of stream. Treating it as EOF tears the link down the instant the chip goes
    quiet, which looks exactly like a flaky bridge.
    """
    total = 0
    try:
        while True:
            data = port.read(4096)
            if not data:
                continue
            conn.sendall(data)
            total += len(data)
    except OSError as exc:
        print(f"[out] {exc}", flush=True)
    print(f"[out] {total} bytes from chip", flush=True)
    try:
        conn.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def serve_one(conn, args, done):
    """Bridge one client session, then release the serial port."""
    port = serial.Serial(args.port, args.baud, timeout=0.05)
    try:
        if not args.no_reset:
            print(f"reset ({args.reset}) into download mode", flush=True)
            RESETS[args.reset](port, args.reset_delay)
        threading.Thread(target=sock_to_port, args=(conn, port),
                         daemon=True).start()
        port_to_sock(port, conn)
    finally:
        try:
            port.close()
        except Exception:  # noqa: BLE001
            pass
        done.set()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default="COM7",
                    help="host serial device (COM7, /dev/ttyUSB0, ...)")
    ap.add_argument("--listen", type=int, default=5000)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--reset", choices=sorted(RESETS), default="classic",
                    help="reset sequence; try 'tight' if the chip will not "
                         "come up in the bootloader (see module docstring)")
    ap.add_argument("--reset-delay", type=float, default=0.1)
    ap.add_argument("--no-reset", action="store_true",
                    help="do not reset at all; use if something else already "
                         "put the chip in the bootloader")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.listen))
    srv.listen(4)
    print(f"bridging {args.port} -> tcp/{args.listen}", flush=True)

    # Only one client may hold the serial port, but accept() must stay responsive
    # or a second session sits unaccepted until esptool's connect times out. A new
    # connection supersedes the old one.
    done = threading.Event()
    done.set()
    active = None

    while True:
        conn, addr = srv.accept()
        print(f"client {addr}", flush=True)

        if not done.is_set():
            print("superseding the previous client", flush=True)
            for closer in (lambda: active.shutdown(socket.SHUT_RDWR),
                           active.close):
                try:
                    closer()
                except OSError:
                    pass
            done.wait(timeout=3.0)

        active = conn
        done.clear()
        threading.Thread(target=serve_one, args=(conn, args, done),
                         daemon=True).start()


if __name__ == "__main__":
    main()
