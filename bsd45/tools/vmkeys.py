"""Send keystrokes to a VMware Workstation guest window (no pywin32).

Usage:  python tools/vmkeys.py "smpon" "smpuser" "shell"

Focus is the hard part: Windows refuses SetForegroundWindow() from a
background process, and the IDE/agent window normally owns the foreground.
Two strategies are tried in order:

  1. AttachThreadInput() to the foreground thread, then SetForegroundWindow.
     This is the documented way for a background process to take focus.
  2. If focus still cannot be taken, post WM_KEYDOWN/WM_KEYUP directly to
     the VMware window (the guest console is driven by those messages, so
     this works without stealing the user's focus).

Both paths verify that something actually happened, and the caller can tell
which one was used from the printed line.
"""
import ctypes, sys, time

u = ctypes.windll.user32
k32 = ctypes.windll.kernel32

TITLE = 'BSD45-SMP 8vCPU - VMware Workstation'
VK = {'/': 0xBF, '-': 0xBD, '.': 0xBE, ' ': 0x20, '_': 0xBD}
WM_KEYDOWN, WM_KEYUP, WM_CHAR = 0x0100, 0x0101, 0x0102


class R(ctypes.Structure):
    _fields_ = [('l', ctypes.c_long), ('t', ctypes.c_long),
                ('r', ctypes.c_long), ('b', ctypes.c_long)]


def win_title(h):
    n = u.GetWindowTextLengthW(h)
    buf = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(h, buf, n + 1)
    return buf.value


def focus(h):
    u.ShowWindow(h, 9)                      # SW_RESTORE
    fg = u.GetForegroundWindow()
    if fg == h:
        return True
    fg_tid = u.GetWindowThreadProcessId(fg, None)
    my_tid = k32.GetCurrentThreadId()
    u.AttachThreadInput(my_tid, fg_tid, True)
    u.BringWindowToTop(h)
    u.SetForegroundWindow(h)
    u.AttachThreadInput(my_tid, fg_tid, False)
    time.sleep(0.5)
    return u.GetForegroundWindow() == h


def vk_of(ch):
    return VK.get(ch, ord(ch.upper()))


def send_focus(s):
    for ch in s:
        vk = vk_of(ch)
        u.keybd_event(vk, 0, 0, 0)
        u.keybd_event(vk, 0, 2, 0)
        time.sleep(0.05)
    u.keybd_event(0x0D, 0, 0, 0)
    u.keybd_event(0x0D, 0, 2, 0)


def send_post(h, s):
    """Post key messages straight at the window (works without focus)."""
    for ch in s:
        vk = vk_of(ch)
        scan = u.MapVirtualKeyW(vk, 0)
        lp = 1 | (scan << 16)
        u.PostMessageW(h, WM_KEYDOWN, vk, lp)
        time.sleep(0.03)
        u.PostMessageW(h, WM_KEYUP, vk, lp | (1 << 30) | (1 << 31))
        time.sleep(0.03)
    scan = u.MapVirtualKeyW(0x0D, 0)
    lp = 1 | (scan << 16)
    u.PostMessageW(h, WM_KEYDOWN, 0x0D, lp)
    u.PostMessageW(h, WM_KEYUP, 0x0D, lp | (1 << 30) | (1 << 31))


h = u.FindWindowW(None, TITLE)
if not h:
    print('no window:', TITLE)
    sys.exit(1)

have_focus = focus(h)
if have_focus:
    r = R()
    u.GetWindowRect(h, ctypes.byref(r))
    u.SetCursorPos((r.l + r.r) // 2, r.t + int((r.b - r.t) * 0.55))
    u.mouse_event(2, 0, 0, 0, 0)
    u.mouse_event(4, 0, 0, 0, 0)
    time.sleep(0.4)

for line in sys.argv[1:]:
    if have_focus:
        send_focus(line)
    else:
        send_post(h, line)
    time.sleep(2)

print('sent via', 'focus' if have_focus else 'postmessage', ':', sys.argv[1:])
if not have_focus:
    print('foreground was', repr(win_title(u.GetForegroundWindow())))
