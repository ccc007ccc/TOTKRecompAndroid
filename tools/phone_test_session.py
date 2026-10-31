#!/usr/bin/env python3
"""Run an automatic 60 FPS benchmark, or control an explicit interactive diagnostic session.

Run in dev after sourcing android/dev_env.sh. Always specify the intended phone.
benchmark loads an isolated save, measures for a fixed duration, then closes the game.
"""
import argparse
import fcntl
import hashlib
import io
import json
import math
import re
import shlex
import subprocess
import tarfile
import time
from pathlib import Path

PACKAGE='com.totkrecomp.android'

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial',required=True)
    ap.add_argument('--package',default=PACKAGE+'.perftest',choices=[PACKAGE+'.debug',PACKAGE+'.perftest'])
    ap.add_argument('--session',help='Private runtime control directory')
    ap.add_argument('--output',type=Path,default=Path('.work/totk-port/performance-phone/session'))
    commands=ap.add_subparsers(dest='command',required=True)
    start=commands.add_parser('start')
    benchmark=commands.add_parser('benchmark',help='Load a save, measure at 60 FPS, then automatically close the game')
    for launch in (start,benchmark):
        launch.add_argument('--apk',type=Path,help='Install this APK while holding the device lease, before launching the test')
        launch.add_argument('--save-seed',help='Existing save root on phone; cloned completely before guest boot')
        launch.add_argument('--game-library',help='Existing private game library path on phone')
        launch.add_argument('--settings-seed',help='Existing settings file on phone; copied into this session before launch')
        launch.add_argument('--shader-warm',choices=['on','off'],help='Enable/disable automatic shader and pipeline preparation for comparison')
        launch.add_argument('--pipeline-warm',choices=['on','off'],help='Enable/disable pipeline preparation while retaining the shader preparation setting')
        launch.add_argument('--submit-snapshot',action='store_true',
                            help='Enable experimental submit-time buffer snapshots for a separate comparison')
        launch.add_argument('--shared-alias',choices=['on','off'],help='Enable/disable GPU-visible resource mappings for a controlled comparison')
        launch.add_argument('--async-buffer-copy',choices=['on','off'],help='Enable/disable deferred GPU buffer copies for a controlled comparison')
        launch.add_argument('--texture-reuse',choices=['on','off'],help='Skip identical registrations in sync mode (default on); with --texture-updates sync, off restores their old queue drain')
        launch.add_argument('--texture-updates',choices=['queued','sync'],help='Queue captured texture descriptor updates, or wait for the renderer before publishing them')
        launch.add_argument('--game-dynres',choices=['default','on','off'],default='default',help='Use the normal runtime policy unless explicitly overridden')
        launch.add_argument('--texture-scaling',choices=['settings','native'],default='settings',help='Respect the selected resolution (default); native bypasses all target scaling for diagnostic comparisons only')
        launch.add_argument('--render-split-us',type=int,help='Long-pass submission interval, 0..20000 microseconds; 0 disables splitting')
        launch.add_argument('--diagnostic-env',action='append',default=[],metavar='NAME=VALUE',
                            help='Diagnostic only: pass one LARECOMP_* measurement switch or Turnip TU_DEBUG/TU_AUTOTUNE_* option; repeatable, recorded in launch.json')
    benchmark.add_argument('--save-slot',type=int,choices=range(6),default=0)
    benchmark.add_argument('--seconds',type=float,default=15,help='Measurement duration after loading, 1..60 seconds (default 15)')
    benchmark.add_argument('--timeout',type=float,help='Overall startup and measurement timeout')
    benchmark.add_argument('--profile',action='store_true',help='Record CPU/GPU timing and constant-buffer copy counters during the automatic run')
    benchmark.add_argument('--shader-stats',action='store_true',help='Diagnostic only: collect driver instruction/register/spill statistics; enables profiling and changes pipeline creation flags')
    benchmark.add_argument('--camera-turn',action='store_true',help='Turn right at full stick during the measured interval; requires a runtime reporting camera_turn_x=32767')
    benchmark.add_argument('--attack-jump',action='store_true',help='During measurement, press Y at 2s and X at 6s of each 8s cycle for 120ms (normal attack/jump with default game controls)')
    benchmark.add_argument('--move-forward',action='store_true',help='Hold the left stick fully forward during measurement; combine with --camera-turn for a circling route')
    gpu_frames=benchmark.add_mutually_exclusive_group()
    gpu_frames.add_argument('--gpu-profile-frame',type=int,help='Diagnostic only: timestamp each GPU pass in this absolute frame; enables profiling')
    gpu_frames.add_argument('--gpu-profile-frames',help='Diagnostic only: comma-separated absolute frames, at most 64; enables profiling')
    benchmark.add_argument('--timing-trace',action='store_true',help='Diagnostic only: trace GPU counters and queue waits; enables profiling')
    benchmark.add_argument('--compare-ubo',type=int,metavar='FRAMES',help='Diagnostic only: alternate copied/shared uniform buffers every 30..600 frames; enables profiling')
    start.add_argument('--diagnostic-present-interval',type=int,choices=[1,2,3],
                       help='Override the game present interval for diagnosis; may change game speed, not a gameplay FPS result')
    boot=start.add_mutually_exclusive_group()
    boot.add_argument('--save-slot',type=int,choices=range(6),help='Load slot_00..slot_05 through the normal menu confirmation flow; requires --save-seed')
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
    launching=args.command in ('start','benchmark')
    timed=args.command=='benchmark'
    if launching and args.render_split_us is not None and not 0<=args.render_split_us<=20000:
        ap.error('render-split-us must be 0..20000')
    if launching:
        names=set()
        for item in args.diagnostic_env:
            name,_,value=item.partition('=')
            # Values travel inside the comma-separated Intent env argument.
            if not re.fullmatch(r'LARECOMP_[A-Z0-9_]+|TU_DEBUG|TU_AUTOTUNE_ALGO|TU_AUTOTUNE_FLAGS',name):
                ap.error('diagnostic-env name must be LARECOMP_*, TU_DEBUG or TU_AUTOTUNE_ALGO/FLAGS')
            if not re.fullmatch(r'[A-Za-z0-9_.:+/-]+',value):ap.error('diagnostic-env value must be one word without commas')
            if name in names:ap.error('diagnostic-env repeats '+name)
            names.add(name)
    if timed:
        if not math.isfinite(args.seconds) or not 1<=args.seconds<=60:ap.error('seconds must be 1..60')
        if args.gpu_profile_frame is not None:
            if not 1<=args.gpu_profile_frame<=1000000:ap.error('gpu-profile-frame must be 1..1000000')
            args.profile=True
        if args.gpu_profile_frames is not None:
            values=args.gpu_profile_frames.split(',')
            if not 1<=len(values)<=64 or any(not re.fullmatch(r'[0-9]{1,7}',v) or not 1<=int(v)<=1000000 for v in values):
                ap.error('gpu-profile-frames needs 1..64 frame numbers in 1..1000000')
            args.gpu_profile_frames=sorted(set(map(int,values)))
            args.profile=True
        if args.timing_trace or args.shader_stats:args.profile=True
        if args.compare_ubo is not None:
            if not 30<=args.compare_ubo<=600:ap.error('compare-ubo must be 30..600 frames')
            args.profile=True
        args.timeout=args.timeout if args.timeout is not None else 135+args.seconds
        if not math.isfinite(args.timeout) or args.timeout<=0:ap.error('timeout must be positive')
        args.auto_load=False;args.diagnostic_present_interval=None
        args.save_seed=args.save_seed or '/data/user/0/'+args.package+'/files/performance/seed'
    if launching and (args.auto_load or args.save_slot is not None) and not args.save_seed:
        ap.error('automatic loading requires an isolated save cloned with --save-seed')
    if args.command=='sample' and not 30<=args.frames<=6000:ap.error('frames must be 30..6000')
    if launching and args.save_seed and any(c in args.save_seed for c in ',\n\r\t '):
        ap.error('save-seed must not contain whitespace or commas (Android environment argument format)')
    if launching and args.game_library and (not args.game_library.startswith('/') or any(c in args.game_library for c in ',\n\r\t ')):
        ap.error('game-library must be an absolute path without whitespace or commas')
    if launching and args.settings_seed and (not args.settings_seed.startswith('/') or any(c in args.settings_seed for c in ',\n\r\t ')):
        ap.error('settings-seed must be an absolute path without whitespace or commas')
    package=args.package
    if launching and args.apk is not None and not args.apk.is_file():ap.error('apk must name an existing APK file')
    device_lock=None
    if launching:
        # All automated launches in this workspace share a device lease. A
        # second controller must not force-stop a benchmark that is still live.
        locks=Path(__file__).resolve().parents[1]/'.work/phone-test-locks'
        locks.mkdir(parents=True,exist_ok=True)
        device_lock=(locks/(hashlib.sha256(args.serial.encode()).hexdigest()+'.lock')).open('a+')
        try:fcntl.flock(device_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('Another automated test is using this device; wait for that controller to finish') from None
        device_lock.seek(0);device_lock.truncate()
        device_lock.write(json.dumps({'package':package,'output':str(args.output),'command':args.command})+'\n');device_lock.flush()
    remote=args.session or '/data/user/0/'+package+'/files/performance/session'
    if timed:
        run_name='benchmark-'+str(time.time_ns())
        remote=(args.session or '/data/user/0/'+package+'/files/performance')+'/'+run_name
        args.output=args.output/run_name
    args.output.mkdir(parents=True,exist_ok=True)
    if not remote.startswith('/data/user/0/'+package+'/files/') or any(c in remote for c in ',\n\r\t '):
        ap.error('session must be a private game files directory without whitespace or commas')

    def adb(*cmd,check=True):
        return subprocess.run(['adb','-s',args.serial,*cmd],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=check)
    def shell(*cmd,check=True):return adb('shell',shlex.join(cmd),check=check)
    def private(*cmd,check=True):return shell('run-as',package,*cmd,check=check)
    def has_process():
        found=shell('pidof',package,check=False)
        if found.returncode and found.stderr.strip():
            raise RuntimeError('Cannot determine whether the game is running: '+found.stderr.strip())
        return bool(found.stdout.strip())
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

    if launching:
        if not timed and shell('pidof',package,check=False).stdout.strip():
            raise RuntimeError('Game is already running; use status/pause/resume, or start this mode on the next launch')
        if args.apk is not None:
            from package_android import apk_application_id
            if apk_application_id(args.apk)!=package:
                raise RuntimeError('APK manifest does not match the selected test package')
            adb('install','-r',str(args.apk.resolve()))
        private('mkdir','-p',remote)
        if not timed:private('rm','-f',remote+'/command',remote+'/status.json')
        env=['LARECOMP_NO_LAUNCHER=1','LARECOMP_TEST_SESSION='+remote,
             'LARECOMP_LOG_FILE='+remote+'/runtime.log','LARECOMP_DISABLE_GPU_CAPTURE=1']
        if args.texture_scaling=='native':env+=['LARECOMP_NO_SCALE=1']
        if args.game_dynres!='default':env+=['LARECOMP_GAME_DYNRES='+('1' if args.game_dynres=='on' else '0')]
        if timed:
            env+=['LARECOMP_TEST_DURATION_SECONDS='+str(args.seconds),'LARECOMP_FPS=60','LARECOMP_NO_USER_INPUT=1']
            if args.camera_turn:env+=['LARECOMP_TEST_CAMERA_TURN=1']
            if args.move_forward:env+=['LARECOMP_TEST_MOVE_FORWARD=1']
            if args.attack_jump:env+=['LARECOMP_TEST_ATTACK_JUMP=1']
        else:env+=['LARECOMP_VK_PROF=1','LARECOMP_LOG_CBUF_BYTES=1','LARECOMP_TEXTURE_TRACE=1','LARECOMP_LOG_FRAMEPACE=120']
        if timed and args.profile:
            env+=['LARECOMP_VK_PROF=1','LARECOMP_LOG_CBUF_BYTES=1','LARECOMP_LOG_PACE=1']
        if timed and args.gpu_profile_frame is not None:env+=['LARECOMP_GPUPROF_AT='+str(args.gpu_profile_frame)]
        if timed and args.gpu_profile_frames is not None:env+=['LARECOMP_GPUPROF_AT='+','.join(map(str,args.gpu_profile_frames))]
        if timed and args.timing_trace:env+=['LARECOMP_TIMING_TRACE=1']
        if timed and args.shader_stats:env+=['LARECOMP_SHADER_STATS=1']
        if timed and args.compare_ubo is not None:
            env+=['LARECOMP_AB='+str(args.compare_ubo)+':ubo']
        if args.submit_snapshot:env+=['LARECOMP_VK_SUBMIT_SNAPSHOT=1','LARECOMP_LOG_SUBMIT_SNAPSHOT=1']
        if args.shared_alias is not None:env+=['LARECOMP_VK_SHARED_ALIAS='+('1' if args.shared_alias=='on' else '0')]
        if args.async_buffer_copy is not None:env+=['LARECOMP_VK_ASYNC_BUFFER_COPY='+('1' if args.async_buffer_copy=='on' else '0')]
        if args.texture_reuse is not None:env+=['LARECOMP_TEXTURE_REUSE='+('1' if args.texture_reuse=='on' else '0')]
        if args.texture_updates is not None:env+=['LARECOMP_TEXTURE_QUEUE='+('1' if args.texture_updates=='queued' else '0')]
        if args.render_split_us is not None:env+=['LARECOMP_VK_SPLIT_US='+str(args.render_split_us)]
        if args.shader_warm is not None:env+=['LARECOMP_SHADER_WARM='+('1' if args.shader_warm=='on' else '0')]
        if args.pipeline_warm is not None:env+=['LARECOMP_PIPELINE_WARM='+('1' if args.pipeline_warm=='on' else '0')]
        settings_copy=None;settings_sha=None
        if args.settings_seed:
            # The runtime can persist driver choices; give every run its own copy.
            settings_copy=remote+'/settings-'+str(time.time_ns())+'.ini'
            private('cp',args.settings_seed,settings_copy)
            settings_sha=private('sha256sum',settings_copy).stdout.split()[0]
            env+=['LARECOMP_SETTINGS='+settings_copy]
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
        controlled={entry.partition('=')[0] for entry in env}
        for item in args.diagnostic_env:
            if item.partition('=')[0] in controlled:
                raise RuntimeError('diagnostic-env cannot replace a controller option: '+item.partition('=')[0])
        env+=args.diagnostic_env
        package_paths=shell('pm','path',package).stdout.strip().splitlines()
        apk=next((p.removeprefix('package:') for p in package_paths if p.endswith('/base.apk')),None)
        apk_sha=shell('sha256sum',apk).stdout.split()[0] if apk else None
        manifest={'serial':args.serial,'package':package,'started_at':time.time(),'env':env,'apk':apk,'apk_sha256':apk_sha,
                  'installed_from':str(args.apk.resolve()) if args.apk is not None else None,
                  'game_library':args.game_library,'game_library_sha256':library_sha,
                  'settings_seed':args.settings_seed,'settings_copy':settings_copy,'settings_sha256':settings_sha,
                  'save_seed':args.save_seed,'save_slot':args.save_slot,
                  'shader_warm':args.shader_warm,'pipeline_warm':args.pipeline_warm,
                  'submit_snapshot':args.submit_snapshot,
                  'shared_alias':args.shared_alias,
                  'async_buffer_copy':args.async_buffer_copy,
                  'texture_reuse':args.texture_reuse,
                  'texture_updates':args.texture_updates,
                  'game_dynres':args.game_dynres,
                  'texture_scaling':args.texture_scaling,
                  'camera_turn':args.camera_turn if timed else False,
                  'camera_turn_x':32767 if timed and args.camera_turn else 0,
                  'move_forward':args.move_forward if timed else False,
                  'attack_jump':args.attack_jump if timed else False,
                  'render_split_us':args.render_split_us,
                  'diagnostic_env':args.diagnostic_env,
                  'profiling':args.profile if timed else True,
                  'gpu_profile_frame':args.gpu_profile_frame if timed else None,
                  'gpu_profile_frames':args.gpu_profile_frames if timed else None,
                  'timing_trace':args.timing_trace if timed else False,
                  'shader_stats':args.shader_stats if timed else False,
                  'ubo_ab_frames':args.compare_ubo if timed else None,
                  'benchmark_seconds':args.seconds if timed else None,
                  'diagnostic_present_interval':args.diagnostic_present_interval}
        (args.output/'launch.json').write_text(json.dumps(manifest,indent=2)+'\n')
        if timed:
            # One autonomous run: no pause/resume commands, screenshots, or model-side confirmation.
            # Native code ends its timer and exits even if the host stops polling.
            shell('am','force-stop',package)
            (args.output/'battery-before.txt').write_text(shell('dumpsys','battery').stdout)
            (args.output/'thermal-before.txt').write_text(shell('dumpsys','thermalservice').stdout)
            state={};result=None;launched=False;closed_by=None
            try:
                shell('am','start','-n',package+'/com.botwrecompandroid.BOTWRecompAndroidActivity','--es','env',','.join(env))
                launched=True
                print(f'Loading slot {args.save_slot}; measuring {args.seconds:g}s at 60 FPS, then closing the game...',flush=True)
                deadline=time.monotonic()+args.timeout
                while time.monotonic()<deadline:
                    time.sleep(.5)
                    try:state=read('status.json')
                    except (subprocess.CalledProcessError,json.JSONDecodeError):
                        if time.time()-manifest['started_at']>5 and not has_process():
                            raise RuntimeError('Benchmark process exited before reporting its status')
                        continue
                    if not state.get('timed') or state.get('target_fps')!=60 or state.get('save_slot')!=args.save_slot:
                        raise RuntimeError('Installed APK does not support this timed 60 FPS benchmark; rebuild/install it')
                    if args.camera_turn and (state.get('camera_turn') is not True or state.get('camera_turn_x')!=32767):
                        raise RuntimeError('Installed APK does not support the requested camera turn; rebuild/install it')
                    if args.move_forward and state.get('move_forward') is not True:
                        raise RuntimeError('Installed APK does not support the requested forward movement; rebuild/install it')
                    if args.attack_jump and state.get('attack_jump') is not True:
                        raise RuntimeError('Installed APK does not support the requested attack/jump sequence; rebuild/install it')
                    if state.get('error'):raise RuntimeError(state['error'])
                    if state['phase']=='ended':
                        if not state.get('ready') or state.get('sample_id')!=1:
                            raise RuntimeError('Benchmark ended without a loaded-scene measurement')
                        result=read('sample-0001.json');break
                    if state['phase'] in ('paused','warming','diagnostic'):
                        raise RuntimeError('Timed benchmark unexpectedly paused or entered an interactive mode')
                    if not has_process():
                        # Exit can race this poll after the final status was atomically published.
                        state=read('status.json')
                        if state['phase']!='ended':raise RuntimeError('Game exited before its measurement completed')
                else:raise RuntimeError('Timed benchmark exceeded its startup/measurement timeout')
                if result['pid']!=state['pid'] or result.get('mode')!='timed-benchmark' or result.get('target_fps')!=60:
                    raise RuntimeError('Benchmark result belongs to a different process or mode')
                if result.get('elapsed_seconds',0)+.001<args.seconds:
                    raise RuntimeError('Benchmark stopped before its requested duration')
                close_deadline=time.monotonic()+2
                while has_process() and time.monotonic()<close_deadline:time.sleep(.1)
                closed_by='controller' if has_process() else 'runtime'
            finally:
                if launched:shell('am','force-stop',package,check=False)
                # Copy only measurement files, never the save tree or game images.
                names=['status.json','runtime.log']+(['sample-0001.json'] if result or state.get('sample_id')==1 else [])
                archive=subprocess.run(['adb','-s',args.serial,'exec-out','run-as',package,'tar','cf','-','-C',remote,*names],capture_output=True)
                if archive.stdout:
                    try:
                        with tarfile.open(fileobj=io.BytesIO(archive.stdout),mode='r:') as tar:
                            for name in names:
                                try:source=tar.extractfile(name)
                                except KeyError:continue
                                if source:(args.output/name).write_bytes(source.read())
                    except tarfile.TarError:pass
                (args.output/'battery-after.txt').write_text(shell('dumpsys','battery',check=False).stdout)
                (args.output/'thermal-after.txt').write_text(shell('dumpsys','thermalservice',check=False).stdout)
                manifest.update(finished_at=time.time(),state=state,closed_by=closed_by)
                (args.output/'launch.json').write_text(json.dumps(manifest,indent=2)+'\n')
            if has_process():raise RuntimeError('Game is still running after the benchmark shutdown')
            result['closed_by']=closed_by
            (args.output/'sample-0001.json').write_text(json.dumps(result,indent=2)+'\n')
            print(f"{result['fps']:.2f} FPS; {result['elapsed_seconds']:.2f}s; p95 {result['p95_ms']:.2f} ms. Game closed.")
            if 'low_1pct_fps' in result:
                print(f"1% low {result['low_1pct_fps']:.2f} FPS; p99 {result['p99_ms']:.2f} ms; max {result['max_ms']:.2f} ms; frames >200 ms: {result['frames_over_200ms']}")
            print(args.output)
            return
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
