#!/usr/bin/env python3
"""Test emulator for God of War III (BCES00510) on an isolated profile (never the real saves), driven through X11.

  gw.py launch <session> [--stock] [--defaults] [--perf] [--cfg=NAME] [--state=NAME] [--ctl="v0 v1 ..."] [NAME=VALUE ...] [CFG:setting=value ...]
        start the fork build (or with --stock the RPCS3 AppImage). Without --defaults every switch the fork turns
        on by itself is set to 0. --cfg picks cfg-NAME.yml (test = capped, bench = no frame limit, savestate),
        --state boots states/NAME.SAVESTAT.zst instead of the game, {session} in a value is the session directory.
  gw.py ready [timeout_s]            wait until the game draws at a steady rate (after a boot or a state load)
  gw.py go <session> <state> [launch arguments]   launch + ready in one step
  gw.py run <session> <state> <seconds> [launch arguments]   boot a state, measure, stop: one result line
  gw.py stop | win | shot <out.png>
  gw.py key <keys> [hold_s]          X11 keysym names, comma separated, held together
  gw.py seq "<step> <step> ..."      steps: key[:seconds] or wait:seconds, e.g. "w:2 wait:1 Next:0.4"
  gw.py save <name>                  Ctrl+S, then keep the new savestate as states/<name>.SAVESTAT.zst
                                     (needs --cfg=savestate and no --defaults, see the fork README)
  gw.py fps <seconds> [label]        title FPS, package power, GPU clock and render thread CPU as one JSON line
  gw.py ctl "<values>"               write live.ctl (see rpcs3/Emu/RSX/VK/VKLiveCtl.hpp)
  gw.py ab <seconds> <reps> "<ctl A>" "<ctl B>" ...   switch live controls inside one boot, one line per arm
ProfilingKeyboard: x cross, c circle, z square, v triangle, Return start, space select, w a s d left stick,
Home End Delete Next right stick (up down left right), e R1, q L1, t R2, r L2, g R3, f L3, arrows d-pad.
CTL_ON / CTL_OFF below are the live control lines for all changes on / off."""
import ctypes as c, glob, json, os, pathlib, re, shutil, signal, statistics, subprocess, sys, time

W = pathlib.Path(__file__).resolve().parent.parent
FORK = pathlib.Path('/home/tiago/Applications/rpcs3/profiling/native-investigation/build-prep/build/bin/rpcs3')
STOCK = pathlib.Path('/home/tiago/Applications/rpcs3/rpcs3.AppImage')
GAME = '/home/tiago/Games/PS3/GodOfWar3/GodOfWar3.iso'
TITLE = 'BCES00510'
STATE = W / 'state.json'
CTL = W / 'live.ctl'
CTL_ON = '1000 65536 1 0 0 0 0 0 1 2 0 1 1'
CTL_OFF = '0 0 0 0 0 0 0 0 0 0 0 0 0'
os.environ.setdefault('DISPLAY', ':0')
# The fork turns these on by itself; a run that should behave like RPCS3 does sets them to 0.
DEFAULT_SWITCHES = ['RPCS3_EXPERIMENT_SPU_INSTCOMBINE', 'RPCS3_SPU_NATIVE_RWV', 'RPCS3_VK_READBACK_ADMISSION', 'RPCS3_VK_READBACK_SHARED_HITS',
                    'RPCS3_VK_READBACK_COMPRESSED_HITS', 'RPCS3_VK_READBACK_STREAM_COPY', 'RPCS3_VK_READBACK_OOP', 'RPCS3_VK_SEMAPHORE_PIPELINE_PREFETCH',
                    'RPCS3_VK_PERIODIC_SUBMIT_US', 'RPCS3_VK_STREAM_DMA_LOAD_MIN', 'RPCS3_EXPERIMENT_BLIT_COMPLEMENT', 'RPCS3_FIFO_INLINE_CACHE',
                    'RPCS3_EXPERIMENT_LAST_IMAGE_VIEW', 'RPCS3_NATIVE_SSAO', 'RPCS3_NATIVE_LIGHTING', 'RPCS3_VK_GEOMETRY_CACHE', 'RPCS3_VK_FAST_DRAWS',
                    'RPCS3_VK_PIPELINE_REUSE', 'RPCS3_VK_DESCRIPTOR_REUSE', 'RPCS3_SPU_XFLOAT_FAST', 'RPCS3_VK_DYNAMIC_FACE', 'RPCS3_NATIVE_AA', 'RPCS3_SPU_PUTLLC_BACKOFF', 'RPCS3_SPU_PUTLLC_PIECEWISE', 'RPCS3_SPU_NATIVE_GEOMETRY', 'RPCS3_RSX_STREAM_VERTEX_COPY', 'RPCS3_SPU_VERTEX_UPLOAD', 'RPCS3_SPURS_RESERVE', 'RPCS3_GOW3_FIVE_SPUS', 'RPCS3_RSX_COPY_WORKER']


def alive(rec):
    try: return pathlib.Path(f"/proc/{rec['pid']}/stat").read_text().split()[21] == rec['start']
    except (OSError, IndexError): return False


def record():
    rec = json.loads(STATE.read_text()) if STATE.exists() else None
    if not rec or not alive(rec): sys.exit('No test emulator running')
    return rec


def window(rec=None):
    tree = subprocess.run(['xwininfo', '-root', '-tree'], capture_output=True, text=True).stdout
    for line in tree.splitlines():
        m = re.match(r'\s*(0x[0-9a-f]+) "(.*)": \("[^"]*" "RPCS3"\)', line)
        if m and TITLE in m[2]: return m[1], m[2]
    return None, None


def title_fps():
    _, t = window(); m = re.match(r'FPS: ([0-9.]+)', t or '')
    return float(m[1]) if m else None


def launch(args):
    if STATE.exists() and alive(json.loads(STATE.read_text())): sys.exit('Previous test emulator still runs')
    if any(subprocess.run(['pgrep', '-x', n], capture_output=True).returncode == 0 for n in ('rpcs3', 'AppRun.wrapped')): sys.exit('Another emulator is running; leave it alone')
    session = W / 'sessions' / args[0]; session.mkdir(parents=True, exist_ok=True)
    opts = {a.split('=', 1)[0]: (a.split('=', 1) + [''])[1] for a in args[1:] if a.startswith('--')}
    stock = '--stock' in opts
    env = {k: v for k, v in os.environ.items() if not k.startswith('RPCS3_') and k not in ('APPIMAGE', 'APPDIR', 'LD_PRELOAD')}
    env.update(XDG_CONFIG_HOME=str(W / 'profile'), XDG_CACHE_HOME=str(W / ('cache-stock' if stock else 'cache')), QT_QPA_PLATFORM='xcb')
    if not stock:
        if '--defaults' not in opts: env.update({k: '0' for k in DEFAULT_SWITCHES})
        CTL.write_text(opts.get('--ctl', CTL_ON if '--defaults' in opts else CTL_OFF) + '\n')
        env['RPCS3_VK_LIVE_CTL'] = str(CTL)
    env.update(dict(a.replace('{session}', str(session)).split('=', 1) for a in args[1:] if '=' in a and not a.startswith(('--', 'CFG:'))))
    cfg = W / f"cfg-{opts.get('--cfg', 'test')}.yml"
    sets = [a[4:].split('=', 1) for a in args[1:] if a.startswith('CFG:')]
    if sets:
        # CFG:<setting>=<value>: one line of the configuration for this run (a setting the file lacks is added under Video)
        text = cfg.read_text()
        for name, value in sets:
            text, n = re.subn(r'(?m)^(\s*' + re.escape(name) + r':).*$', r'\1 ' + value, text)
            if not n: text = text.replace('Video:\n', f'Video:\n  {name}: {value}\n', 1)
        cfg = session / 'config.yml'; cfg.write_text(text)
    boot = str(W / 'states' / (opts['--state'] + '.SAVESTAT.zst')) if '--state' in opts else GAME
    argv = [str(STOCK if stock else FORK), '--no-gui', '--input-config=ProfilingKeyboard', '--config=' + str(cfg), boot]
    # --perf: hold the performance power profile for the session, as the inFamous 2 launcher does
    if '--perf' in opts: argv = ['powerprofilesctl', 'launch', '--profile', 'performance', '--reason', 'RPCS3 test', '--appid', 'rpcs3', *argv]
    log = open(session / 'stdout.log', 'w')
    p = subprocess.Popen(argv, env=env, stdout=log, stderr=log, start_new_session=True)
    time.sleep(1.5)
    emu = subprocess.run(['pgrep', '-x', 'AppRun.wrapped' if stock else 'rpcs3'], capture_output=True, text=True).stdout.split()
    rec = dict(emu=int(emu[0]) if emu else p.pid, pid=p.pid, start=pathlib.Path(f'/proc/{p.pid}/stat').read_text().split()[21], session=str(session), stock=stock,
               log=env['XDG_CACHE_HOME'] + '/rpcs3/RPCS3.log', argv=argv[1:], switches={k: v for k, v in env.items() if k.startswith('RPCS3_')})
    STATE.write_text(json.dumps(rec, indent=1) + '\n'); print('launched', rec['pid'], session.name)


def ready(timeout=600):
    # A loading screen also reports a steady rate; the game is back when the render thread is busy as well.
    rec = record(); end = time.time() + timeout; good = 0; tck = os.sysconf('SC_CLK_TCK')
    while time.time() < end:
        if not alive(rec): sys.exit('Emulator exited; see ' + rec['log'])
        c0 = rsx_ticks(rec['emu']).get('rsx', 0); time.sleep(2); c1 = rsx_ticks(rec['emu']).get('rsx', 0)
        f = title_fps(); good = good + 1 if f and f > 10 and (c1 - c0) / tck / 2 > .12 else 0
        if good >= 3: print('ready', f); return
    sys.exit('Not ready in time')


def keycodes(X, d, keys):
    codes = [X.XKeysymToKeycode(d, X.XStringToKeysym(k.encode())) for k in keys.split(',')]
    if not all(codes): sys.exit('Unknown key in ' + keys)
    return codes


class Key(c.Structure):
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int), ('display', c.c_void_p), ('window', c.c_ulong), ('root', c.c_ulong),
                ('subwindow', c.c_ulong), ('time', c.c_ulong), ('x', c.c_int), ('y', c.c_int), ('x_root', c.c_int), ('y_root', c.c_int),
                ('state', c.c_uint), ('keycode', c.c_uint), ('same_screen', c.c_int)]


class Event(c.Union): _fields_ = [('key', Key), ('pad', c.c_long * 24)]


def send_keys(win, keys, hold, state=0):
    X = c.CDLL('libX11.so.6')
    X.XOpenDisplay.argtypes = [c.c_char_p]; X.XOpenDisplay.restype = c.c_void_p
    X.XDefaultRootWindow.argtypes = [c.c_void_p]; X.XDefaultRootWindow.restype = c.c_ulong
    X.XStringToKeysym.argtypes = [c.c_char_p]; X.XStringToKeysym.restype = c.c_ulong
    X.XKeysymToKeycode.argtypes = [c.c_void_p, c.c_ulong]; X.XKeysymToKeycode.restype = c.c_uint
    X.XSendEvent.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_long, c.c_void_p]; X.XFlush.argtypes = [c.c_void_p]; X.XCloseDisplay.argtypes = [c.c_void_p]
    d = X.XOpenDisplay(None)

    def send(typ, code):
        e = Event(); e.key = Key(type=typ, display=d, window=win, root=X.XDefaultRootWindow(d), time=0, x=1, y=1, same_screen=1, keycode=code, state=state)
        X.XSendEvent(d, win, 0, 1 << (typ - 2), c.byref(e)); X.XFlush(d)
    codes = keycodes(X, d, keys)
    for code in codes: send(2, code)
    time.sleep(hold)
    for code in codes: send(3, code)
    X.XCloseDisplay(d)


def game_window():
    win, _ = window()
    if not win: sys.exit('No game window yet')
    return int(win, 16)


def seq(steps):
    win = game_window()
    for step in steps.split():
        name, _, secs = step.partition(':')
        if name == 'wait': time.sleep(float(secs))
        else: send_keys(win, name, float(secs) if secs else .3); time.sleep(.1)


def save(name):
    rec = record(); win, title = window()
    folder = W / 'profile/rpcs3/savestates'
    before = {p: p.stat().st_mtime for p in folder.rglob('*.SAVESTAT*')} if folder.exists() else {}
    # the window was found by its title above; XFetchName in the helper returns nothing for a title with a trademark sign
    hotkey = FORK.parents[2] / 'source/infamous2/bench/x11_hotkey.py'
    subprocess.run([sys.executable, str(hotkey), win, 's', '--state', '4', '--activate', '--expected-title', ''], check=True)
    end = time.time() + 300
    while time.time() < end:
        new = [p for p in folder.rglob('*.SAVESTAT*') if before.get(p) != p.stat().st_mtime] if folder.exists() else []
        if new and time.time() - new[0].stat().st_mtime > 3:
            (W / 'states').mkdir(exist_ok=True); out = W / 'states' / (name + '.SAVESTAT.zst'); shutil.copy2(new[0], out); new[0].unlink()
            print('saved', out, out.stat().st_size, 'emulator alive:', alive(rec)); return
        if not alive(rec): sys.exit('Emulator died while saving; see ' + rec['log'])
        time.sleep(1)
    sys.exit('No savestate appeared')


def sysfs():
    gpu = next((d for d in sorted(glob.glob('/sys/class/drm/card[0-9]*/device')) if glob.glob(d + '/hwmon/*/freq1_input')), None)
    return glob.glob(gpu + '/hwmon/*')[0] if gpu else None


def rsx_ticks(pid):
    total = {}
    for t in glob.glob(f'/proc/{pid}/task/*'):
        try:
            name = open(t + '/comm').read().strip(); f = open(t + '/stat').read().rsplit(')', 1)[1].split()
        except OSError: continue
        group = 'rsx' if name == 'rsx::thread' else 'spu' if name.startswith('SPU') else 'ppu' if name.startswith('PPU') else 'other'
        total[group] = total.get(group, 0) + int(f[11]) + int(f[12])
    return total


def fps(seconds, label='', quiet=False):
    rec = record(); hw = sysfs(); vals = []; watts = []; clk = []
    t0 = time.time(); c0 = rsx_ticks(rec['emu']); end = t0 + seconds
    while time.time() < end:
        f = title_fps()
        if f is not None: vals.append(f)
        if hw:
            clk.append(int(open(hw + '/freq1_input').read()) / 1e6)
            if os.path.exists(hw + '/power1_average'): watts.append(int(open(hw + '/power1_average').read()) / 1e6)
        time.sleep(.25)
    c1 = rsx_ticks(rec['emu']); dt = (time.time() - t0) * os.sysconf('SC_CLK_TCK')
    if not vals: sys.exit('no fps')
    res = dict(label=label, fps=round(statistics.mean(vals), 2), min=min(vals), max=max(vals), W=round(statistics.mean(watts), 1) if watts else None,
               gpuMHz=round(statistics.mean(clk)) if clk else None, **{k + '_cores': round((c1.get(k, 0) - c0.get(k, 0)) / dt, 2) for k in ('rsx', 'spu', 'ppu', 'other')})
    if not quiet: print(json.dumps(res))
    with open(W / 'results.jsonl', 'a') as f: f.write(json.dumps({**res, 't': time.time(), 'session': rec['session'], 'ctl': CTL.read_text().strip() if CTL.exists() else None}) + '\n')
    return res


def ab(seconds, reps, arms):
    for rep in range(reps):
        for arm in arms:
            CTL.write_text(arm + '\n'); time.sleep(float(os.environ.get('AB_SETTLE', 3.5)))
            r = fps(seconds, 'ab ' + arm, quiet=True)
            while r['spu_cores'] < 3:
                # Kratos died (the fight load is gone): restart from the checkpoint and measure this window again
                send_keys(game_window(), 'x', .06); time.sleep(14)
                r = fps(seconds, 'ab ' + arm, quiet=True)
            print(f"ctl={arm} | fps {r['fps']} min {r['min']} W {r['W']} gpuMHz {r['gpuMHz']} rsx {r['rsx_cores']} spu {r['spu_cores']} ppu {r['ppu_cores']}", flush=True)


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'launch': launch(sys.argv[2:])
    elif cmd == 'go': launch([sys.argv[2], '--state=' + sys.argv[3], *sys.argv[4:]]); time.sleep(3); ready(); time.sleep(4)
    elif cmd == 'run':
        # run <session> <state> <seconds> [launch arguments]: boot, settle, measure, stop; one line
        launch([sys.argv[2], '--state=' + sys.argv[3], *sys.argv[5:]]); time.sleep(3); ready(); time.sleep(4)
        r = fps(float(sys.argv[4]), sys.argv[2], quiet=True); rec = record()
        print(f"{sys.argv[2]} | fps {r['fps']} min {r['min']} W {r['W']} gpuMHz {r['gpuMHz']} rsx {r['rsx_cores']} spu {r['spu_cores']} ppu {r['ppu_cores']}", flush=True)
        os.killpg(rec['pid'], signal.SIGTERM); time.sleep(3)
        if alive(rec): os.killpg(rec['pid'], signal.SIGKILL); time.sleep(1)
    elif cmd == 'ready': ready(float(sys.argv[2]) if len(sys.argv) > 2 else 600)
    elif cmd == 'stop':
        rec = record(); os.killpg(rec['pid'], signal.SIGTERM)
        for _ in range(40):
            if not alive(rec): break
            time.sleep(.25)
        else: os.killpg(rec['pid'], signal.SIGKILL)
        print('stopped')
    elif cmd == 'win': record(); print(*window())
    elif cmd == 'shot': record(); subprocess.run(['import', '-window', window()[0], '-resize', (sys.argv[3] if len(sys.argv) > 3 else '960') + 'x', sys.argv[2]], check=True, timeout=30)
    elif cmd == 'key': record(); send_keys(game_window(), sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else .3)
    elif cmd == 'seq': record(); seq(sys.argv[2])
    elif cmd == 'save': save(sys.argv[2])
    elif cmd == 'fps': fps(float(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else '')
    elif cmd == 'ctl': CTL.write_text(sys.argv[2] + '\n')
    elif cmd == 'revive':
        # the fight is on (restart from the checkpoint if Kratos died); prints the tries it took
        n = 0
        while fps(2, 'revive', quiet=True)['spu_cores'] < 3 and n < 6: send_keys(game_window(), 'x', .06); time.sleep(14); n += 1
        print('revived', n)
    elif cmd == 'ab': ab(float(sys.argv[2]), int(sys.argv[3]), sys.argv[4:])
    else: sys.exit(__doc__)
