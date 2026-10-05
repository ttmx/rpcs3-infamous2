#!/usr/bin/env python3
"""Launch / stop / measure helper for heavy-scene A/B runs.

  run.py launch <binary> [--kb] [--cfg PATH] [--boot PATH] [ENV=VAL ...]   start emulator, print pid + window id
                                                           (boots INFAMOUS2_GAME unless --boot names a game folder or savestate;
                                                           BENCH_PROFILE = directory to use as XDG_CONFIG_HOME)
  run.py stop                                              stop the emulator started by launch
  run.py win                                               print window id of the running emulator
  run.py key <keys> [hold_s]                               send key(s) (comma separated) to game window
  run.py fps <seconds> [label]                             sample title FPS, print summary
  run.py shot <out.png>                                    screenshot the game window (960 wide)
"""
import os, sys, json, time, re, glob, shutil, subprocess, pathlib, signal, statistics, ctypes as c

W = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(W.parent))
import launch as launcher
STATE = W / 'state.json'
os.environ.setdefault('DISPLAY', ':0')


def window(pid=None):
    out = subprocess.run(['xwininfo', '-root', '-tree'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r'\s*(0x[0-9a-f]+) "(.*)": \("rpcs3" "RPCS3"\)', line)
        if m and ('inFamous' in m.group(2) or 'FPS' in m.group(2)):
            return m.group(1), m.group(2)
    return None, None


def launch(args):
    binary = pathlib.Path(args[0]).resolve()
    kb = '--kb' in args
    cfg = W.parent / 'play-config.yml'
    extra = {}
    boot = os.environ.get('INFAMOUS2_GAME')
    it = iter(args[1:])
    for a in it:
        if a == '--cfg':
            cfg = pathlib.Path(next(it)).resolve()
        elif a == '--boot':
            boot = str(pathlib.Path(next(it)).resolve())
        elif '=' in a:
            k, v = a.split('=', 1); extra[k] = v
    if not boot:
        raise SystemExit('nothing to boot: set INFAMOUS2_GAME or pass --boot')
    env = launcher.base_env(launcher.BASE_FLAGS, profile=os.environ.get('BENCH_PROFILE'))
    env.update(extra)
    argv = [str(binary), '--no-gui', '--input-config=' + ('ProfilingKeyboard' if kb else 'Default'), '--config=' + str(cfg), boot]
    # The measurements in docs/INVESTIGATION.md were taken with the performance power profile held.
    power = shutil.which('powerprofilesctl')
    if power:
        argv = [power, 'launch', '--profile', 'performance', '--reason', 'RPCS3 benchmark', '--appid', 'rpcs3', *argv]
    log = open(W / 'launch.log', 'w')
    p = subprocess.Popen(argv, env=env, stdout=log, stderr=log, start_new_session=True)
    end = time.time() + 120
    wid = None
    while time.time() < end and not wid:
        time.sleep(1); wid, _ = window()
    STATE.write_text(json.dumps({'wrapper': p.pid, 'binary': str(binary), 'extra': extra, 'kb': kb, 'cfg': str(cfg), 'window': wid, 't': time.time()}))
    print(json.dumps({'wrapper': p.pid, 'window': wid}))


def emu_pids():
    out = subprocess.run(['pgrep', '-x', 'rpcs3'], capture_output=True, text=True).stdout.split()
    return [int(x) for x in out]


def stop():
    for pid in emu_pids():
        os.kill(pid, signal.SIGTERM)
    end = time.time() + 30
    while time.time() < end and emu_pids():
        time.sleep(0.5)
    for pid in emu_pids():
        os.kill(pid, signal.SIGKILL)
    print('stopped' if not emu_pids() else 'STILL RUNNING')


class Key(c.Structure):
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int), ('display', c.c_void_p), ('window', c.c_ulong), ('root', c.c_ulong), ('subwindow', c.c_ulong), ('time', c.c_ulong), ('x', c.c_int), ('y', c.c_int), ('x_root', c.c_int), ('y_root', c.c_int), ('state', c.c_uint), ('keycode', c.c_uint), ('same_screen', c.c_int)]


class Event(c.Union):
    _fields_ = [('key', Key), ('pad', c.c_long * 24)]


def key(keys, hold=0.15):
    wid, title = window()
    if not wid:
        raise SystemExit('no game window')
    w = int(wid, 16)
    X = c.CDLL('libX11.so.6')
    X.XOpenDisplay.argtypes = [c.c_char_p]; X.XOpenDisplay.restype = c.c_void_p
    X.XDefaultRootWindow.argtypes = [c.c_void_p]; X.XDefaultRootWindow.restype = c.c_ulong
    X.XStringToKeysym.argtypes = [c.c_char_p]; X.XStringToKeysym.restype = c.c_ulong
    X.XKeysymToKeycode.argtypes = [c.c_void_p, c.c_ulong]; X.XKeysymToKeycode.restype = c.c_uint
    X.XSendEvent.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_long, c.c_void_p]
    X.XFlush.argtypes = [c.c_void_p]; X.XCloseDisplay.argtypes = [c.c_void_p]
    d = X.XOpenDisplay(None)
    root = X.XDefaultRootWindow(d)
    codes = [X.XKeysymToKeycode(d, X.XStringToKeysym(k.encode())) for k in keys.split(',')]
    assert all(codes), 'unknown key'

    def send(typ, code):
        e = Event(); e.key = Key(type=typ, display=d, window=w, root=root, time=0, x=1, y=1, same_screen=1, keycode=code, state=0)
        X.XSendEvent(d, w, 0, 1 << (typ - 2), c.byref(e)); X.XFlush(d)
    for code in codes: send(2, code)
    time.sleep(hold)
    for code in reversed(codes): send(3, code)
    X.XCloseDisplay(d)


def gpu_device():
    # First GPU that reports its clock (amdgpu does); elsewhere the GPU columns stay empty.
    for d in sorted(glob.glob('/sys/class/drm/card[0-9]*/device')):
        if glob.glob(d + '/hwmon/*/freq1_input'):
            return d


def mean(values, digits=None):
    return round(statistics.mean(values), digits) if values else None


def fps(seconds, label=''):
    vals = []
    end = time.time() + seconds
    gpu = gpu_device()
    hw = glob.glob(gpu + '/hwmon/*')[0] if gpu else None
    clk = []; temps = []; mhz = []; watts = []
    k10 = next((h for h in glob.glob('/sys/class/hwmon/hwmon*') if open(h + '/name').read().strip() == 'k10temp'), None)
    while time.time() < end:
        _, title = window()
        m = re.search(r'FPS: ([\d.]+)', title or '')
        if m: vals.append(float(m.group(1)))
        if hw:
            clk.append(int(open(hw + '/freq1_input').read()) / 1e6)
            if os.path.exists(hw + '/power1_average'):
                watts.append(int(open(hw + '/power1_average').read()) / 1e6)
        if k10:
            temps.append(int(open(k10 + '/temp1_input').read()) / 1000)
        cur = [int(open(f).read()) for f in glob.glob('/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq')]
        if cur:
            mhz.append(max(cur) / 1000)
        time.sleep(0.25)
    if not vals:
        print('no fps'); return
    vals_sorted = sorted(vals)
    policy = gpu + '/power_dpm_force_performance_level' if gpu else ''
    res = {'label': label, 'W': mean(watts, 1), 'cpuC': round(max(temps)) if temps else None, 'cpuMHz': mean(mhz), 'gpuMHz': mean(clk), 'mean': round(statistics.mean(vals), 2), 'median': round(statistics.median(vals), 2), 'min': vals_sorted[0], 'p10': vals_sorted[len(vals) // 10], 'max': vals_sorted[-1], 'sclk': mean(clk), 'policy': open(policy).read().strip() if os.path.exists(policy) else None}
    print(json.dumps(res))
    with open(W / 'results.jsonl', 'a') as f:
        st = json.loads(STATE.read_text()) if STATE.exists() else {}
        f.write(json.dumps({**res, 't': time.time(), 'binary': st.get('binary'), 'extra': st.get('extra'), 'cfg': st.get('cfg')}) + '\n')


def shot(out):
    wid, _ = window()
    subprocess.run(['import', '-window', wid, '-resize', '960x600', out], check=True)


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'launch': launch(sys.argv[2:])
    elif cmd == 'stop': stop()
    elif cmd == 'win': print(window())
    elif cmd == 'key': key(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 0.15)
    elif cmd == 'fps': fps(float(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else '')
    elif cmd == 'shot': shot(sys.argv[2])
