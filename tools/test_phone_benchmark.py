"""Check the benchmark CLI against a scripted device; never connect to a real phone."""
import json
import fcntl
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FAKE_ADB = r'''#!/usr/bin/env python3
import io, json, os, shlex, sys, tarfile
from pathlib import Path
root = Path(os.environ['PHONE_BENCH_MOCK_DIR'])
a = sys.argv[1:]
assert a[:2] == ['-s', 'phone-benchmark-test'], a
a = a[2:]
with (root/'calls.jsonl').open('a') as f: f.write(json.dumps(a)+'\n')
if a[0]=='install':
    assert a[1]=='-r' and Path(a[2]).is_file();print('Success');sys.exit(0)
p = root/'device.json'
s = json.loads(p.read_text()) if p.exists() else {'running': True, 'polls': 0, 'seconds': 15, 'slot': 0}
mode = os.environ['PHONE_BENCH_MOCK_MODE']
package = 'com.totkrecomp.android.perftest'
def status():
    ended = s['polls'] >= 3 and mode != 'timeout'
    return {'pid':4242,'phase':'ended' if ended else 'sampling' if s['polls']>=2 else 'loading',
        'ready':s['polls']>=2,'sample_id':1 if ended else 0,'save_slot':s['slot'],'target_fps':60,
        'timed':mode!='legacy','camera_turn':s.get('camera_turn',False) and mode!='old-camera',
        'camera_turn_x':(16384 if mode=='old-strength' else 32767) if s.get('camera_turn',False) else 0,
        'attack_jump':s.get('attack_jump',False) and mode!='old-actions',
        'move_forward':s.get('move_forward',False) and mode!='old-forward','requested_seconds':s['seconds'],'error':''}
def result():
    value = {'pid':4242,'frames':int(s['seconds']*60),'fps':60,'mean_ms':1000/60,'p95_ms':1000/60,
        'target_fps':60,'mode':'timed-benchmark','elapsed_seconds':s['seconds'],'requested_seconds':s['seconds']}
    if mode == 'tail-result':
        value.update(low_1pct_fps=9.5,p99_ms=64.2,max_ms=509.8,frames_over_200ms=1,frame_times_ms=[16.0,64.2,12.5,509.8])
    return value
if a[0] == 'exec-out':
    assert a[1:3] == ['run-as',package]
    at = a.index('-C'); names = a[at+2:]
    data = {'status.json':json.dumps(status()).encode(),'runtime.log':b'benchmark mock\n',
            'sample-0001.json':json.dumps(result()).encode()}
    b = io.BytesIO()
    with tarfile.open(fileobj=b,mode='w') as t:
        for n in names:
            item=tarfile.TarInfo(n);item.size=len(data[n]);t.addfile(item,io.BytesIO(data[n]))
    sys.stdout.buffer.write(b.getvalue());sys.exit(0)
assert a[0] == 'shell', a
c = shlex.split(a[1])
if c[:2] == ['pm','path']: print('package:/data/app/perftest/base.apk')
elif c[0] == 'sha256sum': print('f'*64+'  '+c[1])
elif c[:2] == ['am','force-stop']:
    assert c[2] == package;s['running']=False
elif c[:2] == ['am','start']:
    assert c[3] == package+'/com.botwrecompandroid.BOTWRecompAndroidActivity'
    env={};name=None
    for v in c[-1].split(','):
        if '=' in v:
            name,value=v.split('=',1);env[name]=value
        else:
            assert name=='LARECOMP_GPUPROF_AT';env[name]+=','+v
    s['seconds']=float(env['LARECOMP_TEST_DURATION_SECONDS'])
    s['camera_turn']=env.get('LARECOMP_TEST_CAMERA_TURN')=='1'
    s['move_forward']=env.get('LARECOMP_TEST_MOVE_FORWARD')=='1'
    s['attack_jump']=env.get('LARECOMP_TEST_ATTACK_JUMP')=='1'
    s['slot']=int(env['LARECOMP_TEST_SAVE_SLOT']);s['running']=True;s['polls']=0
elif c[0] == 'pidof':
    if s['running']: print('4242')
elif c[:2] == ['dumpsys','battery']: print('  temperature: 300\n  status: 3')
elif c[:2] == ['dumpsys','thermalservice']: print('Thermal Status: 0')
elif c[:2] == ['test','-d']:
    assert c[2] == '/storage/emulated/0/latest-underground-save', c
elif c[:2] == ['run-as',package]:
    if c[2] == 'cp':
        assert c[3] == '/storage/emulated/0/settings-seed.ini', c
        assert c[4].startswith('/data/user/0/'+package+'/files/performance/benchmark-'), c
        s['settings_copy']=c[4]
    elif c[2] == 'sha256sum':
        assert c[3] == s['settings_copy'], c
        print('e'*64+'  '+c[3])
    elif c[2] == 'cat':
        assert c[3].endswith(('status.json','sample-0001.json')), c
        if c[3].endswith('status.json'):
            s['polls']+=1
            if s['polls']>=3 and mode in ('success','tail-result'):s['running']=False
            print(json.dumps(status()))
        else: print(json.dumps(result()))
    else: assert c[2] in ('mkdir','rm','test'), c
else: raise AssertionError(c)
p.write_text(json.dumps(s))
'''


class PhoneBenchmarkTests(unittest.TestCase):
    def setUp(self):
        work = ROOT/'.work/phone-benchmark-tests'
        work.mkdir(parents=True,exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=work)
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        (self.path/'adb').write_text(FAKE_ADB)
        (self.path/'adb').chmod(0o755)

    def run_cli(self, mode, *extra):
        env=dict(os.environ,PHONE_BENCH_MOCK_DIR=str(self.path),PHONE_BENCH_MOCK_MODE=mode)
        env['PATH']=str(self.path)+os.pathsep+env['PATH']
        proc=subprocess.run([sys.executable,str(ROOT/'tools/phone_test_session.py'),
            '--serial','phone-benchmark-test','--output',str(self.path/'reports'),'benchmark',*extra],
            env=env,capture_output=True,text=True,timeout=12)
        calls=[json.loads(line) for line in (self.path/'calls.jsonl').read_text().splitlines()]
        device=json.loads((self.path/'device.json').read_text())
        self.assertFalse(device['running'],proc.stderr)
        launches=[shlex_call[1] for shlex_call in calls if shlex_call[0]=='shell' and shlex_call[1].startswith('am start ')]
        self.assertEqual(len(launches),1)
        self.assertIn('LARECOMP_FPS=60',launches[0])
        profiling=any(option in extra for option in ('--profile','--compare-ubo','--gpu-profile-frame','--gpu-profile-frames','--timing-trace','--shader-stats'))
        self.assertEqual('LARECOMP_VK_PROF=1' in launches[0],profiling)
        self.assertEqual('LARECOMP_LOG_PACE=1' in launches[0],profiling)
        if '--compare-ubo' in extra:
            self.assertIn('LARECOMP_AB='+extra[extra.index('--compare-ubo')+1]+':ubo',launches[0])
        for option, key in [('--shared-alias','LARECOMP_VK_SHARED_ALIAS'),('--async-buffer-copy','LARECOMP_VK_ASYNC_BUFFER_COPY')]:
            if option in extra:
                value=extra[extra.index(option)+1]
                self.assertIn(key+'='+('1' if value=='on' else '0'),launches[0])
            else:self.assertNotIn(key+'=',launches[0])
        self.assertFalse(any(c[0]=='shell' and ('screencap' in c[1] or ('/command' in c[1] and ' rm -f ' not in c[1])) for c in calls))
        self.assertFalse(any(c[0]=='exec-out' and 'window.png' in c for c in calls))
        return proc

    def test_one_launch_measures_default_duration_and_closes_itself(self):
        proc=self.run_cli('success')
        self.assertEqual(proc.returncode,0,proc.stderr)
        reports=list((self.path/'reports').glob('benchmark-*/sample-0001.json'))
        self.assertEqual(len(reports),1)
        sample=json.loads(reports[0].read_text())
        self.assertEqual(sample['requested_seconds'],15)
        self.assertEqual(sample['target_fps'],60)
        self.assertEqual(sample['closed_by'],'runtime')
        self.assertNotIn('1% low',proc.stdout)  # Older runtimes still report their available statistics.

    def test_tail_statistics_are_preserved_and_printed(self):
        proc=self.run_cli('tail-result')
        self.assertEqual(proc.returncode,0,proc.stderr)
        sample=json.loads(next((self.path/'reports').glob('benchmark-*/sample-0001.json')).read_text())
        self.assertEqual(sample['low_1pct_fps'],9.5)
        self.assertEqual(sample['frames_over_200ms'],1)
        self.assertEqual(sample['frame_times_ms'],[16.0,64.2,12.5,509.8])
        self.assertIn('1% low 9.50 FPS; p99 64.20 ms; max 509.80 ms; frames >200 ms: 1',proc.stdout)

    def test_full_circle_attack_jump_route_is_recorded(self):
        proc=self.run_cli('success','--camera-turn','--move-forward','--attack-jump')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertEqual(manifest['camera_turn_x'],32767)
        self.assertTrue(manifest['attack_jump'] and manifest['state']['attack_jump'])
        self.assertIn('LARECOMP_TEST_ATTACK_JUMP=1',manifest['env'])

    def test_old_half_strength_runtime_is_rejected(self):
        proc=self.run_cli('old-strength','--camera-turn')
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('does not support the requested camera turn',proc.stderr)

    def test_runtime_without_attack_jump_is_rejected(self):
        proc=self.run_cli('old-actions','--attack-jump')
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('does not support the requested attack/jump sequence',proc.stderr)

    def test_competing_controller_cannot_force_stop_the_active_test(self):
        folder=ROOT/'.work/phone-test-locks';folder.mkdir(parents=True,exist_ok=True)
        path=folder/(hashlib.sha256(b'phone-benchmark-test').hexdigest()+'.lock')
        with path.open('a+') as lease:
            fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
            env=dict(os.environ,PHONE_BENCH_MOCK_DIR=str(self.path),PHONE_BENCH_MOCK_MODE='success')
            env['PATH']=str(self.path)+os.pathsep+env['PATH']
            proc=subprocess.run([sys.executable,str(ROOT/'tools/phone_test_session.py'),
                '--serial','phone-benchmark-test','--output',str(self.path/'reports'),'benchmark'],
                env=env,capture_output=True,text=True,timeout=5)
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('Another automated test',proc.stderr)
        self.assertFalse((self.path/'calls.jsonl').exists())

    def test_controller_closes_a_runtime_that_does_not_exit(self):
        proc=self.run_cli('controller','--seconds','1')
        self.assertEqual(proc.returncode,0,proc.stderr)
        report=next((self.path/'reports').glob('benchmark-*/sample-0001.json'))
        self.assertEqual(json.loads(report.read_text())['closed_by'],'controller')

    def test_warm_comparison_uses_a_private_settings_copy(self):
        proc=self.run_cli('success','--settings-seed','/storage/emulated/0/settings-seed.ini',
                          '--shader-warm','on','--pipeline-warm','off')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertNotEqual(manifest['settings_seed'],manifest['settings_copy'])
        self.assertEqual(manifest['settings_sha256'],'e'*64)
        self.assertIn('LARECOMP_SETTINGS='+manifest['settings_copy'],manifest['env'])
        self.assertIn('LARECOMP_SHADER_WARM=1',manifest['env'])
        self.assertIn('LARECOMP_PIPELINE_WARM=0',manifest['env'])
        self.assertEqual(manifest['texture_scaling'],'settings')
        self.assertEqual(manifest['game_dynres'],'default')
        self.assertFalse(any(e.startswith(('LARECOMP_NO_SCALE=','LARECOMP_GAME_DYNRES=')) for e in manifest['env']))

    def test_native_resolution_comparison_is_explicit(self):
        proc=self.run_cli('success','--texture-scaling','native','--game-dynres','on')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertEqual(manifest['texture_scaling'],'native')
        self.assertIn('LARECOMP_NO_SCALE=1',manifest['env'])
        self.assertIn('LARECOMP_GAME_DYNRES=1',manifest['env'])

    def test_camera_turn_is_explicit_and_keeps_resolution_settings(self):
        proc=self.run_cli('success','--camera-turn')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertTrue(manifest['camera_turn'])
        self.assertIn('LARECOMP_TEST_CAMERA_TURN=1',manifest['env'])
        self.assertNotIn('LARECOMP_NO_SCALE=1',manifest['env'])

    def test_runtime_without_camera_route_is_refused(self):
        proc=self.run_cli('old-camera','--camera-turn')
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('does not support the requested camera turn',proc.stderr)

    def test_forward_and_camera_route_preserves_selected_save_and_settings(self):
        seed='/storage/emulated/0/latest-underground-save'
        proc=self.run_cli('success','--camera-turn','--move-forward','--save-seed',seed,'--save-slot','0')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertTrue(manifest['camera_turn'] and manifest['move_forward'])
        self.assertEqual(manifest['save_seed'],seed)
        self.assertEqual(manifest['save_slot'],0)
        self.assertIn('LARECOMP_TEST_MOVE_FORWARD=1',manifest['env'])
        self.assertIn('LARECOMP_TEST_CAMERA_TURN=1',manifest['env'])
        self.assertNotIn('LARECOMP_NO_SCALE=1',manifest['env'])

    def test_runtime_without_forward_route_is_refused(self):
        proc=self.run_cli('old-forward','--camera-turn','--move-forward')
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('does not support the requested forward movement',proc.stderr)

    def test_profiling_remains_a_single_automatic_run(self):
        proc=self.run_cli('success','--profile')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertTrue(manifest['profiling'])
        self.assertIsNone(manifest['ubo_ab_frames'])

    def test_ubo_comparison_records_alternation_and_enables_profiling(self):
        proc=self.run_cli('success','--compare-ubo','60')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertTrue(manifest['profiling'])
        self.assertEqual(manifest['ubo_ab_frames'],60)

    def test_gpu_diagnostics_are_explicit_and_recorded(self):
        proc=self.run_cli('success','--gpu-profile-frame','1150','--timing-trace')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertTrue(manifest['profiling'])
        self.assertTrue(manifest['timing_trace'])
        self.assertEqual(manifest['gpu_profile_frame'],1150)
        self.assertIn('LARECOMP_GPUPROF_AT=1150',manifest['env'])
        self.assertIn('LARECOMP_TIMING_TRACE=1',manifest['env'])

    def test_gpu_frame_list_preserves_following_launch_options(self):
        proc=self.run_cli('success','--gpu-profile-frames','1230,1150,1230','--timing-trace','--camera-turn','--move-forward')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertEqual(manifest['gpu_profile_frames'],[1150,1230])
        self.assertIsNone(manifest['gpu_profile_frame'])
        self.assertTrue(manifest['profiling'] and manifest['timing_trace'])
        self.assertIn('LARECOMP_GPUPROF_AT=1150,1230',manifest['env'])
        self.assertEqual(manifest['state']['save_slot'],0)
        self.assertTrue(manifest['state']['move_forward'])

    def test_invalid_gpu_frame_lists_are_rejected_before_adb(self):
        for value in ('','0','1000001','1,','1,-2','1,abc',','.join(['1']*65)):
            proc=subprocess.run([sys.executable,str(ROOT/'tools/phone_test_session.py'),'--serial','phone-benchmark-test',
                'benchmark','--gpu-profile-frames',value],capture_output=True,text=True,timeout=5)
            self.assertNotEqual(proc.returncode,0,value)
            self.assertIn('gpu-profile-frames needs',proc.stderr)
        self.assertFalse((self.path/'calls.jsonl').exists())

    def test_gpu_profile_frame_rejected_before_touching_device(self):
        for frame in ('0','-1','1000001'):
            proc=subprocess.run([sys.executable,str(ROOT/'tools/phone_test_session.py'),'--serial','phone-benchmark-test',
                'benchmark','--gpu-profile-frame',frame],capture_output=True,text=True,timeout=5)
            self.assertNotEqual(proc.returncode,0)
            self.assertIn('gpu-profile-frame must be',proc.stderr)
        self.assertFalse((self.path/'calls.jsonl').exists())

    def test_shader_statistics_are_explicit_and_marked_as_profiling(self):
        proc=self.run_cli('success','--shader-stats')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertTrue(manifest['profiling'])
        self.assertTrue(manifest['shader_stats'])
        self.assertIn('LARECOMP_SHADER_STATS=1',manifest['env'])

    def test_diagnostic_environment_is_forwarded_and_recorded(self):
        proc=self.run_cli('success','--diagnostic-env','LARECOMP_VK_NO_MUTABLE=1','--diagnostic-env','TU_DEBUG=sysmem')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertFalse(manifest['profiling'])
        self.assertEqual(manifest['diagnostic_env'],['LARECOMP_VK_NO_MUTABLE=1','TU_DEBUG=sysmem'])
        self.assertIn('LARECOMP_VK_NO_MUTABLE=1',manifest['env'])
        self.assertIn('TU_DEBUG=sysmem',manifest['env'])

    def test_diagnostic_environment_rejected_before_touching_device(self):
        for bad in (['PATH=/tmp'],['LD_PRELOAD=x.so'],['LARECOMP_X=a,b'],['TU_DEBUG=a b'],['LARECOMP_X='],
                    ['LARECOMP_X=1','LARECOMP_X=0']):
            options=[part for item in bad for part in ('--diagnostic-env',item)]
            proc=subprocess.run([sys.executable,str(ROOT/'tools/phone_test_session.py'),'--serial','phone-benchmark-test',
                'benchmark',*options],capture_output=True,text=True,timeout=5)
            self.assertNotEqual(proc.returncode,0,bad)
            self.assertIn('diagnostic-env',proc.stderr)
        self.assertFalse((self.path/'calls.jsonl').exists())

    def test_diagnostic_environment_cannot_replace_controller_options(self):
        env=dict(os.environ,PHONE_BENCH_MOCK_DIR=str(self.path),PHONE_BENCH_MOCK_MODE='success')
        env['PATH']=str(self.path)+os.pathsep+env['PATH']
        proc=subprocess.run([sys.executable,str(ROOT/'tools/phone_test_session.py'),'--serial','phone-benchmark-test',
            '--output',str(self.path/'reports'),'benchmark','--diagnostic-env','LARECOMP_FPS=30'],
            env=env,capture_output=True,text=True,timeout=12)
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('cannot replace a controller option: LARECOMP_FPS',proc.stderr)
        calls=[json.loads(line) for line in (self.path/'calls.jsonl').read_text().splitlines()]
        self.assertFalse(any(c[0]=='shell' and c[1].startswith('am start ') for c in calls))

    def test_render_split_zero_is_forwarded_without_enabling_profiling(self):
        proc=self.run_cli('success','--render-split-us','0')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertFalse(manifest['profiling'])
        self.assertEqual(manifest['render_split_us'],0)
        self.assertIn('LARECOMP_VK_SPLIT_US=0',manifest['env'])

    def test_texture_reuse_comparison_is_recorded_without_profiling(self):
        proc=self.run_cli('success','--texture-reuse','off','--texture-updates','queued','--game-dynres','off')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertFalse(manifest['profiling'])
        self.assertEqual(manifest['texture_reuse'],'off')
        self.assertIn('LARECOMP_TEXTURE_REUSE=0',manifest['env'])
        self.assertEqual(manifest['texture_updates'],'queued')
        self.assertIn('LARECOMP_TEXTURE_QUEUE=1',manifest['env'])
        self.assertEqual(manifest['game_dynres'],'off')
        self.assertIn('LARECOMP_GAME_DYNRES=0',manifest['env'])

    def test_old_runtime_is_refused_and_closed_without_sampling(self):
        proc=self.run_cli('legacy','--seconds','1')
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('does not support',proc.stderr)
        self.assertFalse(list((self.path/'reports').glob('benchmark-*/sample-0001.json')))

    def test_memory_optimization_switches_are_recorded(self):
        proc=self.run_cli('success','--shared-alias','on','--async-buffer-copy','off')
        self.assertEqual(proc.returncode,0,proc.stderr)
        manifest=json.loads(next((self.path/'reports').glob('benchmark-*/launch.json')).read_text())
        self.assertEqual(manifest['shared_alias'],'on')
        self.assertEqual(manifest['async_buffer_copy'],'off')

    def test_apk_install_is_part_of_the_locked_launch(self):
        apk=self.path/'test.apk';apk.write_bytes(b'test')
        aapt=self.path/'sdk/build-tools/36.0.0/aapt';aapt.parent.mkdir(parents=True)
        aapt.write_text("#!/bin/sh\necho \"package: name='com.totkrecomp.android.perftest'\"\n");aapt.chmod(0o755)
        old=os.environ.get('ANDROID_HOME');os.environ['ANDROID_HOME']=str(self.path/'sdk')
        try:proc=self.run_cli('success','--apk',str(apk))
        finally:
            if old is None:os.environ.pop('ANDROID_HOME',None)
            else:os.environ['ANDROID_HOME']=old
        self.assertEqual(proc.returncode,0,proc.stderr)
        calls=[json.loads(s) for s in (self.path/'calls.jsonl').read_text().splitlines()]
        self.assertEqual(sum(c[0]=='install' for c in calls),1)
        self.assertLess(next(i for i,c in enumerate(calls) if c[0]=='install'),next(i for i,c in enumerate(calls) if c[0]=='shell' and c[1].startswith('am start ')))

    def test_startup_timeout_closes_the_game(self):
        proc=self.run_cli('timeout','--seconds','1','--timeout','1')
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('timeout',proc.stderr)


if __name__=='__main__':unittest.main()
