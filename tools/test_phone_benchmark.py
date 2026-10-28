"""Check the benchmark CLI against a scripted device; never connect to a real phone."""
import json
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
p = root/'device.json'
s = json.loads(p.read_text()) if p.exists() else {'running': True, 'polls': 0, 'seconds': 15, 'slot': 0}
mode = os.environ['PHONE_BENCH_MOCK_MODE']
package = 'com.totkrecomp.android.perftest'
def status():
    ended = s['polls'] >= 3 and mode != 'timeout'
    return {'pid':4242,'phase':'ended' if ended else 'sampling' if s['polls']>=2 else 'loading',
        'ready':s['polls']>=2,'sample_id':1 if ended else 0,'save_slot':s['slot'],'target_fps':60,
        'timed':mode!='legacy','requested_seconds':s['seconds'],'error':''}
def result():
    return {'pid':4242,'frames':int(s['seconds']*60),'fps':60,'mean_ms':1000/60,'p95_ms':1000/60,
        'target_fps':60,'mode':'timed-benchmark','elapsed_seconds':s['seconds'],'requested_seconds':s['seconds']}
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
    env=dict(v.split('=',1) for v in c[-1].split(','));s['seconds']=float(env['LARECOMP_TEST_DURATION_SECONDS'])
    s['slot']=int(env['LARECOMP_TEST_SAVE_SLOT']);s['running']=True;s['polls']=0
elif c[0] == 'pidof':
    if s['running']: print('4242')
elif c[:2] == ['dumpsys','battery']: print('  temperature: 300\n  status: 3')
elif c[:2] == ['dumpsys','thermalservice']: print('Thermal Status: 0')
elif c[:2] == ['run-as',package]:
    if c[2] == 'cat':
        assert c[3].endswith(('status.json','sample-0001.json')), c
        if c[3].endswith('status.json'):
            s['polls']+=1
            if s['polls']>=3 and mode=='success':s['running']=False
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
        self.assertNotIn('LARECOMP_VK_PROF=',launches[0])
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

    def test_controller_closes_a_runtime_that_does_not_exit(self):
        proc=self.run_cli('controller','--seconds','1')
        self.assertEqual(proc.returncode,0,proc.stderr)
        report=next((self.path/'reports').glob('benchmark-*/sample-0001.json'))
        self.assertEqual(json.loads(report.read_text())['closed_by'],'controller')

    def test_old_runtime_is_refused_and_closed_without_sampling(self):
        proc=self.run_cli('legacy','--seconds','1')
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('does not support',proc.stderr)
        self.assertFalse(list((self.path/'reports').glob('benchmark-*/sample-0001.json')))

    def test_startup_timeout_closes_the_game(self):
        proc=self.run_cli('timeout','--seconds','1','--timeout','1')
        self.assertNotEqual(proc.returncode,0)
        self.assertIn('timeout',proc.stderr)


if __name__=='__main__':unittest.main()
