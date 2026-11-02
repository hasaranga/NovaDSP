# comcap.py - read a Pico's USB CDC port without being able to wedge on it.
#
# Written to SESSION.md's prescription, which is there because the obvious
# implementations cost replugs:
#
#   * **.NET SerialPort.Open() blocks forever on this device**, and a reader
#     stuck in a pending ReadFile cannot be killed - it holds the COM port
#     until the board is physically replugged. So: Win32 CreateFile/ReadFile
#     directly, with COMMTIMEOUTS set so ReadFile always returns.
#   * **DTR must be asserted before reading** or the capture comes back empty
#     and looks like a dead board.
#   * **Bound by wall clock, not by read timeout**, with a watchdog thread on
#     os._exit() so nothing can hang past the deadline.
#
#   python comcap.py COM17 15

import ctypes, os, sys, threading, time
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
SETDTR = 5


class DCB(ctypes.Structure):
    _fields_ = [("DCBlength", wintypes.DWORD), ("BaudRate", wintypes.DWORD),
                ("fBits", wintypes.DWORD),
                ("wReserved", wintypes.WORD), ("XonLim", wintypes.WORD),
                ("XoffLim", wintypes.WORD), ("ByteSize", ctypes.c_ubyte),
                ("Parity", ctypes.c_ubyte), ("StopBits", ctypes.c_ubyte),
                ("XonChar", ctypes.c_char), ("XoffChar", ctypes.c_char),
                ("ErrorChar", ctypes.c_char), ("EofChar", ctypes.c_char),
                ("EvtChar", ctypes.c_char), ("wReserved1", wintypes.WORD)]


class COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [("ReadIntervalTimeout", wintypes.DWORD),
                ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
                ("ReadTotalTimeoutConstant", wintypes.DWORD),
                ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
                ("WriteTotalTimeoutConstant", wintypes.DWORD)]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM17"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

    # The watchdog is the whole safety story: whatever happens below, the
    # process is gone at the deadline and the handle goes with it.
    def bomb():
        time.sleep(secs + 3.0)
        sys.stderr.write("\n[comcap] watchdog fired\n")
        sys.stderr.flush()
        os._exit(2)
    threading.Thread(target=bomb, daemon=True).start()

    h = k32.CreateFileW(r"\\.\%s" % port, GENERIC_READ | GENERIC_WRITE,
                        0, None, OPEN_EXISTING, 0, None)
    if h == INVALID_HANDLE_VALUE:
        sys.stderr.write("[comcap] cannot open %s: %d\n"
                         % (port, ctypes.get_last_error()))
        os._exit(1)

    dcb = DCB()
    dcb.DCBlength = ctypes.sizeof(DCB)
    if not k32.GetCommState(h, ctypes.byref(dcb)):
        sys.stderr.write("[comcap] GetCommState failed\n")
        os._exit(1)
    dcb.BaudRate = 115200          # ignored by CDC, set anyway
    dcb.ByteSize, dcb.Parity, dcb.StopBits = 8, 0, 0
    # fBinary | fDtrControl=DTR_CONTROL_ENABLE. The DTR bit is the one that
    # matters; without it the capture is empty and reads as a dead board.
    dcb.fBits = 0x01 | (0x01 << 4)
    k32.SetCommState(h, ctypes.byref(dcb))
    k32.EscapeCommFunction(h, SETDTR)

    to = COMMTIMEOUTS(50, 0, 200, 0, 0)     # ReadFile always returns
    k32.SetCommTimeouts(h, ctypes.byref(to))

    buf = ctypes.create_string_buffer(4096)
    got = wintypes.DWORD(0)
    end = time.time() + secs
    total = 0

    out = sys.stdout.buffer
    while time.time() < end:
        if not k32.ReadFile(h, buf, 4096, ctypes.byref(got), None):
            break
        if got.value:
            out.write(buf.raw[:got.value])
            out.flush()
            total += got.value

    k32.CloseHandle(h)
    sys.stderr.write("\n[comcap] %d bytes in %.0f s\n" % (total, secs))
    os._exit(0)


main()
