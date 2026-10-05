"""Isolated, identity-checked test launcher: boots a savestate on a separate RPCS3 profile (profile/, a copy of your
RPCS3 configuration folder laid out as profile/rpcs3) with keyboard input, and sends keys / takes screenshots.

  live_test.py launch <session dir> [NAME=VALUE | CFG:setting=value ...]
  live_test.py status | stop | shot <out.png> | key <keys> [hold_s]
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT.parent))
import launch as launcher
import run
os.environ.setdefault('DISPLAY', ':0')


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def pid_record(pid):
    p=Path('/proc')/str(pid)
    # A rebuild replaces the binary on disk; the running process then reports its path as deleted.
    return dict(pid=pid, exe=str((p/'exe').resolve()).removesuffix(' (deleted)'), start_ticks=(p/'stat').read_text().split()[21])
def verified(record):
    try: return pid_record(record['pid']) == {k:record[k] for k in ('pid','exe','start_ticks')}
    except (OSError, IndexError): return False


def game_window(record):
    if not verified(record): return None,None
    tree=subprocess.run(['xwininfo','-root','-tree'],capture_output=True,text=True,check=True).stdout
    for line in tree.splitlines():
        m=re.match(r'\s*(0x[0-9a-f]+) "(.*)": \("rpcs3" "RPCS3"\)',line)
        if not m: continue
        prop=subprocess.run(['xprop','-id',m[1],'_NET_WM_PID'],capture_output=True,text=True).stdout
        if re.search(r'=\s*'+str(record['pid'])+r'\b',prop): return m[1],m[2]
    return None,None


def serve(label, extra=()):
    # The parent keeps the performance lane leased for this emulator session.
    lock=open(ROOT/'performance-lane.lock','a')
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    if subprocess.run(['pgrep','-x','rpcs3'],capture_output=True).returncode==0:
        raise RuntimeError('An emulator is already running; preserve it')
    # LIVE_TEST_BOOT: boot something other than the dock state (the game directory, another savestate)
    # LIVE_TEST_BINARY: test another build than the one launch.py finds
    binary=launcher.find_binary(os.environ.get('LIVE_TEST_BINARY'))
    digest=sha(binary)
    session=ROOT/label;session.mkdir(parents=True,exist_ok=True)
    # LIVE_TEST_CACHE: keep one cache directory across builds that do not change the PPU/SPU recompilers
    cache=ROOT/os.environ.get('LIVE_TEST_CACHE','cache-'+digest[:12]);cache.mkdir(exist_ok=True)
    # Fresh cache, exact binary marker, and no imported native objects.
    (cache/'binary-sha256').write_text(digest+'\n')
    # A savestate does not resume on an empty SPU cache: seed the guest SPU bytecode list from the play cache.
    seed=launcher.cache_home(os.environ.get('LIVE_TEST_SEED_CACHE'))
    found=sorted(seed.glob('rpcs3/cache/BCES01143/ppu-*-EBOOT.BIN/spu-mega-v1-tane.dat'))
    raw_source=found[0] if found else None
    raw_target=cache/raw_source.relative_to(seed) if found else None
    if raw_target and not raw_target.exists():
        raw_target.parent.mkdir(parents=True,exist_ok=True)
        raw_target.write_bytes(raw_source.read_bytes())
        (session/'guest-bytecode-cache.json').write_text(json.dumps(dict(source=str(raw_source),sha256=sha(raw_source),kind='guest SPU bytecode, no native host objects'),indent=2)+'\n')
    cfg=ROOT/'play-config.yml'
    text=(ROOT.parent/'play-config.yml').read_text()
    # CFG:<setting>=<value> arguments override one line of the playable configuration for this test only.
    for name,value in (e[4:].split('=',1) for e in extra if e.startswith('CFG:')):
        import re as _re
        text,n=_re.subn(r'(?m)^(\s*'+_re.escape(name)+r':).*$',r'\1 '+value,text)
        if n!=1: raise RuntimeError('Setting not found exactly once: '+name)
    extra=[e for e in extra if not e.startswith('CFG:')]
    cfg.write_text(text)
    profile=ROOT/'profile/rpcs3'
    if not profile.is_dir(): raise RuntimeError('No test profile: copy your RPCS3 configuration folder to '+str(profile))
    (profile/'vfs.yml').write_text('"$(EmulatorDir)": "'+str(profile)+'/"\n')
    (ROOT/'live.ctl').write_text('1000 65536 1 0 0 0 0\n')
    env=launcher.base_env(launcher.BASE_FLAGS,cache=cache,profile=ROOT/'profile')
    env.update(RPCS3_NATIVE_SSAO='5',RPCS3_VK_LIVE_CTL=str(ROOT/'live.ctl'),RPCS3_VK_PERIODIC_SUBMIT_US='1000',
               RPCS3_EXPERIMENT_BLIT_COMPLEMENT='1',RPCS3_FIFO_INLINE_CACHE='1',RPCS3_EXPERIMENT_LAST_IMAGE_VIEW='1')
    # Extra NAME=VALUE flags; {session} expands to this run's directory.
    env.update(dict(e.replace('{session}',str(session)).split('=',1) for e in extra))
    argv=[str(binary),'--no-gui','--input-config=ProfilingKeyboard','--config='+str(cfg),os.environ.get('LIVE_TEST_BOOT',str(ROOT/'states/dock.SAVESTAT.zst'))]
    # LIVE_TEST_WRAP: a command prefix for the emulator (diagnostics, e.g. strace), with {session} expanded
    argv=[*os.environ.get('LIVE_TEST_WRAP','').replace('{session}',str(session)).split(),*argv]
    argv=['powerprofilesctl','launch','--profile','performance','--reason','RPCS3 lighting diagnostic','--appid','rpcs3',*argv]
    with open(session/'stdout.log','w') as log:
        p=subprocess.Popen(argv,env=env,stdout=log,stderr=log,start_new_session=True)
        (ROOT/'lease.json').write_text(json.dumps(dict(pid=os.getpid(),child=p.pid,label=label)))
        end=time.time()+60
        while time.time()<end and p.poll() is None:
            for proc in Path('/proc').iterdir():
                if not proc.name.isdigit(): continue
                try:
                    rec=pid_record(int(proc.name))
                    if rec['exe'] != str(binary): continue
                except (OSError,IndexError): continue
                gpu=run.gpu_device()
                rec.update(binary_sha256=digest,label=label,config_sha256=sha(cfg),cache=str(cache),profile=str(profile),
                           gpu_policy=Path(gpu+'/power_dpm_force_performance_level').read_text().strip() if gpu else None)
                (ROOT/'state.json').write_text(json.dumps(rec,indent=2)+'\n')
                (session/'provenance.json').write_text(json.dumps(rec,indent=2)+'\n')
                break
            if (ROOT/'state.json').exists(): break
            time.sleep(.2)
        p.wait()


def launch(label,extra=()):
    old=json.loads((ROOT/'state.json').read_text()) if (ROOT/'state.json').exists() else None
    if old and verified(old): raise RuntimeError('Previous test emulator still runs')
    (ROOT/'state.json').unlink(missing_ok=True)
    with open(ROOT/'lease.log','w') as log:
        subprocess.Popen([sys.executable,str(Path(__file__).resolve()),'serve',label,*extra],stdout=log,stderr=log,start_new_session=True)
    end=time.time()+20
    while time.time()<end:
        if (ROOT/'state.json').exists():
            print((ROOT/'state.json').read_text());return
        time.sleep(.2)
    raise RuntimeError('Launch identity not recorded; inspect lease.log before retrying')


def stop(record):
    if not verified(record): raise RuntimeError('Emulator identity no longer matches')
    os.kill(record['pid'],signal.SIGTERM)
    end=time.time()+30
    while time.time()<end and verified(record): time.sleep(.2)
    if verified(record): os.kill(record['pid'],signal.SIGKILL)
    # Wait for the owning launcher to release the performance lane.
    with open(ROOT/'performance-lane.lock','a') as lane:
        fcntl.flock(lane,fcntl.LOCK_EX)
    print('Stopped verified test emulator')


def main():
    cmd=sys.argv[1]
    if cmd=='serve': return serve(sys.argv[2],sys.argv[3:])
    if cmd=='launch': return launch(sys.argv[2],sys.argv[3:])
    record=json.loads((ROOT/'state.json').read_text())
    if cmd=='stop': return stop(record)
    win,title=game_window(record)
    if cmd=='status': print(json.dumps(dict(verified=verified(record),window=win,title=title,record=record),indent=2))
    elif cmd=='shot':
        if not win: raise RuntimeError('No verified game window')
        subprocess.run(['import','-window',win,'-resize','1280x720',sys.argv[2]],check=True)
    elif cmd=='key':
        if not win: raise RuntimeError('No verified game window')
        run.window=lambda pid=None:(win,title)
        run.key(sys.argv[2],float(sys.argv[3]) if len(sys.argv)>3 else .15)


if __name__=='__main__': main()
