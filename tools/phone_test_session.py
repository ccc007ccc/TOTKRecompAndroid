#!/usr/bin/env python3
"""Control a persistent Android performance session without restarting between samples.

Run in dev after sourcing android/dev_env.sh. Always specify the intended phone.
Initial loading still occurs once; a ready scene pauses so it can cool first.
"""
import argparse
import io
import json
import shlex
import subprocess
import tarfile
import time
from pathlib import Path

PACKAGE='com.totkrecomp.android'

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial',required=True)
    ap.add_argument('--package',default=PACKAGE,choices=[PACKAGE,PACKAGE+'.perftest'])
    ap.add_argument('--session',help='Private runtime control directory')
    ap.add_argument('--output',type=Path,default=Path('.work/totk-port/performance-phone/session'))
    commands=ap.add_subparsers(dest='command',required=True)
    start=commands.add_parser('start')
    start.add_argument('--save-seed',help='Existing save root on phone; cloned completely before guest boot')
    start.add_argument('--game-library',help='Existing game library path on phone (for isolated O0/O1 comparisons)')
    start.add_argument('--diagnostic-present-interval',type=int,choices=[1,2,3],
                       help='Override the game present interval for diagnosis; may change game speed, not a gameplay FPS result')
    boot=start.add_mutually_exclusive_group()
    boot.add_argument('--save-slot',type=int,choices=range(6),help='Directly load slot_00..slot_05 using the original game load sequence; requires --save-seed')
    boot.add_argument('--auto-load',action='store_true',help='Legacy button-based startup; prefer --save-slot')
    start.add_argument('--timeout',type=int,default=150,help='Maximum seconds to wait for automatic pause')
    for name in ['status','pause','resume','a','gpu','shot']:
        commands.add_parser(name)
    fps=commands.add_parser('fps',help='Change the target while paused, without reloading or writing settings')
    fps.add_argument('rate',type=int,choices=[0,30,45,60,120],help='0 restores the normal setting/environment')
    sample=commands.add_parser('sample')
    sample.add_argument('--frames',type=int,default=120)
    sample.add_argument('--cooldown',type=int,default=0,help='Wait this many seconds while already paused, then sample')
    sample.add_argument('--timeout',type=int,default=180)
    args=ap.parse_args()
    if args.command=='start' and (args.auto_load or args.save_slot is not None) and not args.save_seed:
        ap.error('automatic loading requires an isolated save cloned with --save-seed')
    if args.command=='sample' and not 30<=args.frames<=6000:ap.error('frames must be 30..6000')
    if args.command=='start' and args.save_seed and any(c in args.save_seed for c in ',\n\r\t '):
        ap.error('save-seed must not contain whitespace or commas (Android environment argument format)')
    if args.command=='start' and args.game_library and (not args.game_library.startswith('/') or any(c in args.game_library for c in ',\n\r\t ')):
        ap.error('game-library must be an absolute path without whitespace or commas')
    args.output.mkdir(parents=True,exist_ok=True)
    package=args.package
    remote=args.session or '/data/user/0/'+package+'/files/performance/session'
    if not remote.startswith('/data/user/0/'+package+'/files/') or any(c in remote for c in ',\n\r\t '):
        ap.error('session must be a private game files directory without whitespace or commas')

    def adb(*cmd,check=True):
        return subprocess.run(['adb','-s',args.serial,*cmd],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=check)
    def shell(*cmd,check=True):return adb('shell',shlex.join(cmd),check=check)
    def private(*cmd,check=True):return shell('run-as',package,*cmd,check=check)
    def read(name):return json.loads(private('cat',remote+'/'+name).stdout)
    def foreground():
        lines=shell('dumpsys','activity','activities').stdout.splitlines()
        resumed=[line.strip() for line in lines if 'topResumedActivity=' in line]
        if not resumed:resumed=[line.strip() for line in lines if 'ResumedActivity' in line]
        return any(package+'/' in line for line in resumed)
    def send(verb,arg=0):
        ident=time.time_ns()//1000000
        # Fixed paths and numeric commands, quoted as separate shell arguments.
        script='printf %s '+shlex.quote(f'{ident} {verb} {arg}\n')+' > '+shlex.quote(remote+'/command.partial')+' && mv '+shlex.quote(remote+'/command.partial')+' '+shlex.quote(remote+'/command')
        private('sh','-c',script)
        for _ in range(50):
            try:
                state=read('status.json')
                if state['request']==ident:
                    if state['error'] and verb!='status':raise RuntimeError(state['error'])
                    return state
            except subprocess.CalledProcessError:pass
            time.sleep(.1)
        raise RuntimeError('Runtime did not acknowledge the command; check that test-session mode is running')

    if args.command=='start':
        if shell('pidof',package,check=False).stdout.strip():
            raise RuntimeError('Game is already running; use status/pause/resume, or start this mode on the next launch')
        private('mkdir','-p',remote)
        private('rm','-f',remote+'/command',remote+'/status.json')
        env=['LARECOMP_NO_LAUNCHER=1','LARECOMP_TEST_SESSION='+remote,
             'LARECOMP_VK_PROF=1','LARECOMP_LOG_CBUF_BYTES=1','LARECOMP_TEXTURE_TRACE=1',
             'LARECOMP_LOG_FILE='+remote+'/runtime.log','LARECOMP_DISABLE_GPU_CAPTURE=1',
             'LARECOMP_LOG_FRAMEPACE=120',
             'LARECOMP_NO_SCALE=1','LARECOMP_GAME_DYNRES=1']
        if args.save_seed:
            check_seed=private if args.save_seed.startswith('/data/user/0/'+package+'/') else shell
            check_seed('test','-d',args.save_seed)
            save=remote+'/save-'+str(time.time_ns()//1000000)
            env+=['LARECOMP_TEST_SAVE_SEED='+args.save_seed,'LARECOMP_SAVE_DIR='+save]
        if args.auto_load:env+=['LARECOMP_TEST_AUTOLOAD=1']
        if args.save_slot is not None:env+=['LARECOMP_TEST_SAVE_SLOT='+str(args.save_slot)]
        if args.diagnostic_present_interval is not None:
            env+=['LARECOMP_FORCE_INTERVAL='+str(args.diagnostic_present_interval),'LARECOMP_LOG_PACE=1']
        library_sha=None
        if args.game_library:
            private('test','-r',args.game_library)
            library_sha=private('sha256sum',args.game_library).stdout.split()[0]
            env+=['LARECOMP_GAME_LIB='+args.game_library]
        package_paths=shell('pm','path',package).stdout.strip().splitlines()
        apk=next((p.removeprefix('package:') for p in package_paths if p.endswith('/base.apk')),None)
        apk_sha=shell('sha256sum',apk).stdout.split()[0] if apk else None
        manifest={'serial':args.serial,'package':package,'started_at':time.time(),'env':env,'apk':apk,'apk_sha256':apk_sha,
                  'game_library':args.game_library,'game_library_sha256':library_sha,
                  'diagnostic_present_interval':args.diagnostic_present_interval}
        (args.output/'launch.json').write_text(json.dumps(manifest,indent=2)+'\n')
        print(shell('am','start','-n',package+'/com.botwrecompandroid.BOTWRecompAndroidActivity','--es','env',','.join(env)).stdout.strip())
        print('Waiting for the loaded scene to pause; runtime also stops startup after 120 seconds...',flush=True)
        deadline=time.monotonic()+args.timeout
        last_error_check=0
        while time.monotonic()<deadline:
            time.sleep(1)
            if time.monotonic()-last_error_check>=3:
                last_error_check=time.monotonic()
                log=private('tail','-c','8192',remote+'/runtime.log',check=False).stdout
                fatal=next((line for line in log.splitlines() if line.startswith('FATAL:')),None)
                if fatal:
                    (args.output/'startup-error.log').write_text(log)
                    raise RuntimeError(fatal)
            try:state=read('status.json')
            except (subprocess.CalledProcessError,json.JSONDecodeError):continue
            if args.save_slot is not None and state.get('save_slot')!=args.save_slot:
                raise RuntimeError('Installed APK does not report the requested direct-save slot; check its build before testing')
            if state['phase']=='paused':
                manifest.update(paused_at=time.time(),state=state)
                (args.output/'launch.json').write_text(json.dumps(manifest,indent=2)+'\n')
                print(json.dumps(state,indent=2))
                if state.get('error') or not state.get('ready',True):
                    raise RuntimeError('Startup did not reach the test view: '+state.get('error',''))
                print(f'Paused after {manifest["paused_at"]-manifest["started_at"]:.1f}s. Use shot to verify the view, then sample.')
                return
        raise RuntimeError('Startup did not reach a paused frame in time; inspect status/log before sampling')
        return
    running=shell('pidof',package,check=False).stdout.split()
    if not running:raise RuntimeError('Selected game package is not running; saved status is not a live session')
    state=send('status')
    if str(state['pid']) not in running:raise RuntimeError('Control directory belongs to a different game process')
    if args.command=='sample':
        if state['phase']!='paused':raise RuntimeError('Pause at the chosen test view before sampling')
        if not state.get('ready',True):raise RuntimeError('Startup has not reached the test scene')
        if state.get('error'):raise RuntimeError('Resolve the session error before sampling: '+state['error'])
        if args.cooldown:
            print(f'Cooling in the same session for {args.cooldown}s...',flush=True)
            time.sleep(max(0,args.cooldown))
        if not foreground():raise RuntimeError('Bring the selected game package to the foreground before sampling')
        expected=state['sample_id']+1
        result_pid=state['pid']
        thermal_before=shell('dumpsys','thermalservice').stdout
        (args.output/f'sample-{expected:04}-thermal-before.txt').write_text(thermal_before)
        (args.output/f'sample-{expected:04}-battery-before.txt').write_text(shell('dumpsys','battery').stdout)
        send('sample',args.frames)
        print(f'Sampling {args.frames} frames after 30 warm-up frames...',flush=True)
        deadline=time.monotonic()+args.timeout
        while time.monotonic()<deadline:
            time.sleep(1)
            state=read('status.json')
            if state['pid']!=result_pid:raise RuntimeError('Game process changed during sampling')
            if state['sample_id']>=expected and state['phase']=='paused':
                result=read(f'sample-{expected:04}.json')
                result['foreground_after']=foreground()
                launch=args.output/'launch.json'
                if launch.exists():
                    manifest=json.loads(launch.read_text())
                    interval=manifest.get('diagnostic_present_interval')
                    if interval is not None and manifest.get('state',{}).get('pid')==result_pid:
                        result['mode']='present-interval-diagnostic'
                        result['diagnostic_present_interval']=interval
                (args.output/f'sample-{expected:04}.json').write_text(json.dumps(result,indent=2)+'\n')
                (args.output/'runtime.log').write_text(private('cat',remote+'/runtime.log').stdout)
                (args.output/f'sample-{expected:04}-thermal-after.txt').write_text(shell('dumpsys','thermalservice').stdout)
                (args.output/f'sample-{expected:04}-battery-after.txt').write_text(shell('dumpsys','battery').stdout)
                if not result['foreground_after']:
                    raise RuntimeError('Game left the foreground during sampling; result retained but must not be used for comparison')
                print(json.dumps(result,indent=2));return
            if state['phase'] in ('paused','ended'):raise RuntimeError('Sample interrupted before completion')
        raise RuntimeError('Sample timed out; use pause/status to inspect the still-running session')
    elif args.command!='status':
        was_paused=state['phase']=='paused'
        if args.command=='shot':private('rm','-f',remote+'/window.png')
        state=send(args.command,args.rate if args.command=='fps' else 0)
        if args.command in ('shot','gpu') and was_paused:
            deadline=time.monotonic()+30
            while state['phase']!='paused' and time.monotonic()<deadline:
                time.sleep(.2);state=read('status.json')
            if state['phase']!='paused':raise RuntimeError('Diagnostic did not finish in 30 seconds')
            if args.command=='shot':
                target=args.output/f'view-{state["frame"]}.png'
                archive=subprocess.run(['adb','-s',args.serial,'exec-out','run-as',package,
                                        'tar','cf','-','-C',remote,'window.png'],check=True,capture_output=True).stdout
                with tarfile.open(fileobj=io.BytesIO(archive),mode='r:') as tar:
                    target.write_bytes(tar.extractfile('window.png').read())
                print(target)
            else:
                (args.output/'runtime.log').write_text(private('cat',remote+'/runtime.log').stdout)
    print(json.dumps(state,indent=2))

if __name__=='__main__':main()
