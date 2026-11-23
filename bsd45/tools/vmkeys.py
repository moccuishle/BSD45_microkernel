import ctypes, sys, time

u = ctypes.windll.user32
TITLE = 'BSD45-SMP 8vCPU - VMware Workstation'
VK = {'/': 0xBF, '-': 0xBD, '.': 0xBE, ' ': 0x20, '_': 0xBD}

class R(ctypes.Structure):
    _fields_ = [('l', ctypes.c_long), ('t', ctypes.c_long),
                ('r', ctypes.c_long), ('b', ctypes.c_long)]

h = u.FindWindowW(None, TITLE)
if not h:
    print('no window')
    sys.exit(1)
u.ShowWindow(h, 9); u.SetForegroundWindow(h); time.sleep(0.8)
r = R(); u.GetWindowRect(h, ctypes.byref(r))
u.SetCursorPos((r.l + r.r) // 2, (r.t + r.b) // 2)
u.mouse_event(2, 0, 0, 0, 0); u.mouse_event(4, 0, 0, 0, 0); time.sleep(0.6)

def send(s):
    for ch in s:
        vk = VK.get(ch, ord(ch.upper()))
        u.keybd_event(vk, 0, 0, 0); u.keybd_event(vk, 0, 2, 0)
        time.sleep(0.05)
    u.keybd_event(0x0D, 0, 0, 0); u.keybd_event(0x0D, 0, 2, 0)

for line in sys.argv[1:]:
    send(line)
    time.sleep(float(sys.argv[0] and 2))
print('sent')
