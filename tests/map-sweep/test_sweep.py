import json
from pathlib import Path
import tempfile
import struct
import unittest
from unittest.mock import patch
import numpy as np
from PIL import Image
from compare import compare, measures
from capture import selected, ERROR, backend_problems, engine_log, log_errors, png_problems, run_game
from ghosts import duration
from ci import negative_control, baseline, shard_count, plan, merge, find_parts, unstable_maps
from renderers import RENDERERS, SIZES


class SweepTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for side in ('old', 'new'):
            p = self.root / side
            (p / 'frames').mkdir(parents=True)
            (p / 'manifest.json').write_text(json.dumps(dict(environment={'image': '1'}, commit=side, renderer='test', failures=[])))
            Image.new('RGB', (20, 20), 'black').save(p / 'frames/a.png')

    def run_compare(self):
        return compare(self.root / 'old', self.root / 'new', self.root / 'report')

    def test_identical(self):
        r = self.run_compare()
        self.assertFalse(r['red'])
        self.assertEqual(r['identical'], 1)

    def test_change_red_and_crops(self):
        Image.new('RGB', (20, 20), 'white').save(self.root / 'new/frames/a.png')
        self.assertTrue(self.run_compare()['red'])
        self.assertTrue((self.root / 'report/a-difference.png').exists())

    def test_changed_environment_is_informational(self):
        Image.new('RGB', (20, 20), 'white').save(self.root / 'new/frames/a.png')
        p = self.root / 'new/manifest.json'
        m = json.loads(p.read_text()); m['environment']['image'] = '2'; p.write_text(json.dumps(m))
        r = self.run_compare()
        self.assertTrue(r['environment_changed'])
        self.assertFalse(r['red'])

    def test_a_new_runner_image_with_the_same_graphics_stack_is_still_compared(self):
        for side, version in (('old', '127.1'), ('new', '370.1')):
            p = self.root / side / 'manifest.json'
            m = json.loads(p.read_text())
            m['environment'] = {'ImageOS': 'ubuntu24', 'ImageVersion': version, 'RR_DRIVER': 'mesa 25.2.8',
                                'RR_CPU': 'EPYC ' + version}
            p.write_text(json.dumps(m))
        Image.new('RGB', (20, 20), 'white').save(self.root / 'new/frames/a.png')
        r = self.run_compare()
        self.assertFalse(r['environment_changed'])
        self.assertTrue(r['red'])
        self.assertIn('Runner images: 370.1; baseline\'s: 127.1', (self.root / 'report/summary.md').read_text())
        # the same images with a new Mesa are not
        p = self.root / 'new/manifest.json'
        m = json.loads(p.read_text()); m['environment']['RR_DRIVER'] = 'mesa 25.3.0'; p.write_text(json.dumps(m))
        r = self.run_compare()
        self.assertTrue(r['environment_changed'])
        self.assertFalse(r['red'])

    def test_environment_is_tracked_per_map(self):
        for side in ('old', 'new'):
            p = self.root / side / 'manifest.json'
            m = json.loads(p.read_text())
            m['map_environments'] = {'a': {'image': side}, 'b': {'image': 'stable'}}
            p.write_text(json.dumps(m))
        Image.new('RGB', (20, 20), 'white').save(self.root / 'new/frames/a.png')
        self.assertFalse(self.run_compare()['red'])
        Image.new('RGB', (20, 20), 'black').save(self.root / 'old/frames/b.png')
        Image.new('RGB', (20, 20), 'white').save(self.root / 'new/frames/b.png')
        self.assertTrue(self.run_compare()['red'])

    def test_known_unstable_maps_are_never_red_but_are_reported(self):
        Image.new('RGB', (20, 20), 'white').save(self.root / 'new/frames/a.png')
        r = compare(self.root / 'old', self.root / 'new', self.root / 'report', unstable={'a': 'differs every run'})
        self.assertFalse(r['red'])
        self.assertEqual(r['frames'][0]['status'], 'changed')
        text = (self.root / 'report/summary.md').read_text()
        self.assertIn('differs every run', text)
        self.assertIn('(unstable)', text)
        # health and inventory failures of an unstable map still count
        (self.root / 'new/frames/a.png').rename(self.root / 'new/frames/b.png')
        self.assertTrue(compare(self.root / 'old', self.root / 'new', self.root / 'report2', unstable={'a': 'x'})['red'])

    def test_unstable_list_names_real_maps_and_renderers(self):
        folder = Path(__file__).parent
        maps = {g.split('-')[0] for g in (folder / 'ghosts.txt').read_text().splitlines()}
        for renderer, entry in json.loads((folder / 'unstable.json').read_text()).items():
            self.assertIn(renderer, RENDERERS)
            self.assertTrue(entry['reason'])
            self.assertLessEqual(set(entry['maps']), maps)
            self.assertEqual(set(unstable_maps(renderer)), set(entry['maps']))
        self.assertEqual(unstable_maps('software'), {})

    def test_missing_extra_always_red(self):
        (self.root / 'new/frames/a.png').rename(self.root / 'new/frames/b.png')
        r = self.run_compare()
        self.assertTrue(r['red'])
        self.assertEqual({x['status'] for x in r['frames']}, {'missing', 'extra'})

    def test_real_frame_negative_control(self):
        negative_control(self.root / 'new', self.root / 'proof')
        result = json.loads((self.root / 'proof/negative-control/report.json').read_text())
        self.assertTrue(result['red'])
        self.assertEqual(result['frames'][0]['plain_pct'], 100)

    def test_first_baseline(self):
        self.assertFalse(compare(None, self.root / 'new', self.root / 'report')['red'])

    def test_health_red_without_baseline(self):
        p = self.root / 'new/manifest.json'
        m = json.loads(p.read_text()); m['failures'] = ['timeout']; p.write_text(json.dumps(m))
        self.assertTrue(compare(None, self.root / 'new', self.root / 'report')['red'])

    def test_threshold(self):
        # One distant color in 10,000 pixels is below both thresholds.
        a = np.zeros((100, 100, 3), dtype=np.int16); b = a.copy(); b[50, 50] = 255
        plain, structural, _, _ = measures(a, b)
        self.assertAlmostEqual(plain, .01)
        self.assertAlmostEqual(structural, .01)
        Image.fromarray(a.astype('uint8')).save(self.root / 'old/frames/a.png')
        Image.fromarray(b.astype('uint8')).save(self.root / 'new/frames/a.png')
        self.assertFalse(self.run_compare()['red'])

    def test_edge_measure(self):
        a = np.zeros((20, 20, 3), dtype=np.int16); a[:, 10:] = 255
        b = np.zeros_like(a); b[:, 11:] = 255
        plain, structural, _, _ = measures(a, b)
        self.assertGreater(plain, 0); self.assertEqual(structural, 0)

    def test_selection(self):
        ghosts = ['RR_A-skin-person', 'RR_B-skin-person']
        self.assertEqual(selected(ghosts, '', 1, 2), ghosts[1:])
        with self.assertRaises(ValueError):
            selected(ghosts, 'RR_BAD', 0, 1)

    def test_official_duration_layouts(self):
        for version in (14, 15, 16, 17, 18):
            b = bytearray(100)
            b[:12] = b'\xf0KartReplay\x0f'
            struct.pack_into('<H', b, 14, version)
            b[96:100] = b'PLAY'
            b += b'RR_TEST\0' + bytes(16) + struct.pack('<H', 1)
            b += b'Race\0' + bytes([3, 0])  # laps, no add-ons
            b += bytes([1]) if version < 16 else struct.pack('<H', 1)
            b += bytes(22 + (32 if version < 16 else 16))
            b += struct.pack('<I', 1234)
            self.assertEqual(duration(b), 1234)
        with self.assertRaises(ValueError):
            duration(bytes(200))

    def test_schedules_and_complete_sharding(self):
        folder = Path(__file__).parent
        ghosts = (folder / 'ghosts.txt').read_text().splitlines()
        self.assertEqual(len(ghosts), 213)
        split = [g for i in range(8) for g in selected(ghosts, '', i, 8)]
        self.assertEqual(set(split), set(ghosts))
        self.assertEqual(len(split), 213)
        schedules = json.loads((folder / 'schedules.json').read_text())
        self.assertEqual(len(schedules), 50)
        for mapname, entry in schedules.items():
            self.assertIn(mapname, {g.split('-')[0] for g in ghosts})
            self.assertEqual(entry['tics'], sorted(set(entry['tics'])))
            self.assertEqual(len(entry['tics']), 5)
            self.assertLess(entry['recorded_race_tics'], 2135)

    def test_branch_capture_never_becomes_master_baseline(self):
        def fake_api(path):
            if 'workflows/' in path:
                return {'workflow_runs': [{'id': 1}, {'id': 2}]}
            return {'artifacts': [{'name': 'baseline-test', 'expired': False,
                                   'eligible': '/runs/2/' in path}]}
        def fake_download(artifact, dest):
            dest.mkdir()
            (dest / 'manifest.json').write_text(json.dumps({
                'full': True, 'failures': [], 'baseline_eligible': artifact['eligible'], 'size': [320, 200]}))
        with patch.dict('os.environ', GITHUB_REPOSITORY='test/test', GITHUB_RUN_ID='99'), \
             patch('ci.api', side_effect=fake_api), patch('ci.download', side_effect=fake_download):
            self.assertEqual(baseline('test', self.root / 'previous', '320x200'), '2')

    def test_backend_assertions(self):
        vulkan = 'VibeRant backend: Vulkan\nVulkan: device: llvmpipe (LLVM 20), validation on\n'
        self.assertEqual(backend_problems('vulkan', vulkan), [])
        self.assertTrue(backend_problems('vulkan', 'VID_StartupOpenGL()\n'))
        d3d = ('VibeRant backend: D3D12\nD3D12 debug layer enabled\nusing WARP for testing\n'
               "D3D12: device: adapter 'Microsoft Basic Render Driver' 1414:008C\n")
        self.assertEqual(backend_problems('d3d', d3d), [])
        self.assertTrue(backend_problems('d3d', d3d.replace('using WARP for testing', '')))
        self.assertEqual(backend_problems('gl', 'VID_StartupOpenGL()...\n'), [])
        self.assertTrue(backend_problems('gl', 'VID_StartupOpenGL()...\nVibeRant backend: Vulkan\n'))
        self.assertEqual(backend_problems('software', 'Under software\n'), [])
        self.assertTrue(backend_problems('software', 'VID_StartupOpenGL()...\n'))

    def test_png_validation(self):
        good = self.root / 'good.png'
        Image.new('RGB', (64, 36)).save(good)
        self.assertEqual(png_problems(good, 64, 36), [])
        self.assertTrue(png_problems(good, 640, 360))
        bad = self.root / 'bad.png'
        bad.write_bytes(b'not a png at all, but long enough')
        self.assertTrue(png_problems(bad, 64, 36))

    def test_run_game_outcomes(self):
        import sys
        import time

        def run(code, timeout=30, stall=30):
            with patch('capture.POLL', 0.1), patch('capture.STOP_GRACE', 0.3):
                return run_game([sys.executable, '-c', code], self.root, None, self.root / 'game.log', timeout, stall)
        self.assertEqual(run('print("fine")'), 0)
        self.assertEqual(run('import sys; sys.exit(3)'), 3)
        # A game waiting at its error dialog is killed after a moment, not at the deadline.
        started = time.monotonic()
        self.assertEqual(run('import time; print("I_Error(): x", flush=True); time.sleep(60)'), 'stopped')
        self.assertLess(time.monotonic() - started, 20)
        self.assertEqual(run('import time; time.sleep(60)', timeout=1), 'timeout')
        self.assertEqual(run('import time; time.sleep(60)', stall=1), 'hang')
        # A slow game is not a hung one: frames keep arriving, so only the overall deadline applies.
        slow = 'import time\nfor i in range(5):\n    print("framecapture: tic", i, flush=True); time.sleep(0.6)'
        self.assertEqual(run(slow, stall=2), 0)

    def test_baseline_must_have_the_same_size(self):
        def fake_api(path):
            if 'workflows/' in path:
                return {'workflow_runs': [{'id': 1}]}
            return {'artifacts': [{'name': 'baseline-test', 'expired': False}]}
        def fake_download(artifact, dest):
            dest.mkdir()
            (dest / 'manifest.json').write_text(json.dumps({
                'full': True, 'failures': [], 'baseline_eligible': True, 'size': [320, 200]}))
        with patch.dict('os.environ', GITHUB_REPOSITORY='test/test', GITHUB_RUN_ID='99'), \
             patch('ci.api', side_effect=fake_api), patch('ci.download', side_effect=fake_download):
            self.assertEqual(baseline('test', self.root / 'previous', '1280x720'), '')
            self.assertEqual(baseline('test', self.root / 'previous2', '320x200'), '1')

    def test_shard_planning(self):
        for renderer in RENDERERS:
            for size in SIZES:
                count = shard_count(renderer, size, 213)
                self.assertGreaterEqual(count, 1)
                # A shard's expected time stays at or under the planner's target
                self.assertLessEqual(213 / count * RENDERERS[renderer]['seconds'][size], 45 * 60)
        self.assertEqual(shard_count('vulkan', '1280x720', 2), 1)
        self.assertEqual(shard_count('vulkan', '1280x720', 20), 3)
        self.assertEqual(shard_count('software', '320x200', 1), 1)

    def test_subset_is_split_evenly(self):
        ghosts = [f'RR_{c}-skin-person' for c in 'ABCDEFGH']
        got = [selected(ghosts, 'RR_B,RR_D,RR_F,RR_H', i, 2) for i in range(2)]
        self.assertEqual([len(x) for x in got], [2, 2])

    def test_summary_is_capped(self):
        for side in ('old', 'new'):
            p = self.root / side
            for i in range(10):
                Image.new('RGB', (20, 20), 'black' if side == 'old' else 'white').save(p / 'frames' / f'f{i}.png')
        compare(self.root / 'old', self.root / 'new', self.root / 'report', limit=3)
        text = (self.root / 'report/summary.md').read_text()
        self.assertEqual(text.count('| f'), 3)
        self.assertIn('more frames', text)

    def run_plan(self, runs=(), marker=False, **env):
        out = self.root / 'output.txt'
        out.write_text('')

        def fake_api(path):
            if path.startswith('repos/test/test/commits/'):
                return {'sha': 'abc123'}
            if 'workflows/' in path:
                return {'workflow_runs': list(runs)}
            return {'artifacts': [{'name': 'master-sweep-abc123' if marker else 'other'}]}
        base = dict(GITHUB_REPOSITORY='test/test', GITHUB_RUN_ID='99', GITHUB_OUTPUT=str(out),
                    GITHUB_EVENT_NAME='workflow_dispatch', GITHUB_REF='refs/heads/master')
        with patch.dict('os.environ', {**base, **env}), patch('ci.api', side_effect=fake_api):
            plan()
        return dict(line.split('=', 1) for line in out.read_text().splitlines())

    def test_plan_for_a_branch_run(self):
        got = self.run_plan(INPUT_REF='feature', INPUT_RENDERERS='vulkan,d3d', INPUT_SIZE='1280x720',
                            GITHUB_REF='refs/heads/feature')
        self.assertEqual(got['master'], 'false')
        self.assertEqual(got['skip'], 'false')
        self.assertEqual(json.loads(got['builds']), ['linux', 'windows'])
        matrix = json.loads(got['matrix'])['include']
        self.assertEqual({m['renderer'] for m in matrix}, {'vulkan', 'd3d'})
        self.assertEqual({m['os'] for m in matrix if m['renderer'] == 'd3d'}, {'windows-2022'})
        self.assertEqual(len(matrix), sum(json.loads(got['shards']).values()))

    def test_plan_for_master_and_a_few_maps(self):
        got = self.run_plan(INPUT_REF='master', INPUT_MAPS='RR_AERIALHIGHLANDS, rr_ramppark')
        self.assertEqual(got['master'], 'true')
        self.assertEqual(got['maps'], 'RR_AERIALHIGHLANDS,RR_RAMPPARK')
        self.assertTrue(all(n <= 2 for n in json.loads(got['shards']).values()))
        # master's ref run from another branch's workflow is not master's
        self.assertEqual(self.run_plan(INPUT_REF='master', GITHUB_REF='refs/heads/feature')['master'], 'false')

    def test_plan_rejects_what_the_game_cannot_do(self):
        for bad in (dict(INPUT_RENDERERS='software,metal'), dict(INPUT_SIZE='640x360'), dict(INPUT_MAPS='RR_NOPE')):
            with self.assertRaises(ValueError):
                self.run_plan(**bad)

    def test_nightly_runs_only_when_master_moved(self):
        night = dict(GITHUB_EVENT_NAME='schedule')
        done = [{'id': 5, 'head_sha': 'abc123'}]
        self.assertEqual(self.run_plan(runs=done, marker=True, **night)['skip'], 'true')
        # the same commit without a complete sweep, or a different commit, runs
        self.assertEqual(self.run_plan(runs=done, marker=False, **night)['skip'], 'false')
        self.assertEqual(self.run_plan(runs=[{'id': 5, 'head_sha': 'old'}], marker=True, **night)['skip'], 'false')
        # this run itself doesn't count as an earlier one
        self.assertEqual(self.run_plan(runs=[{'id': 99, 'head_sha': 'abc123'}], marker=True, **night)['skip'], 'false')
        # a manual run never skips
        self.assertEqual(self.run_plan(runs=done, marker=True)['skip'], 'false')

    def test_shards_are_found_by_manifest_in_either_download_layout(self):
        def part(folder, renderer):
            (folder / 'frames').mkdir(parents=True)
            (folder / 'manifest.json').write_text(json.dumps(dict(renderer=renderer)))
        single, several = self.root / 'one', self.root / 'many'
        part(single, 'vulkan')  # one artifact is extracted straight into the folder
        part(several / 'frames-d3d-0', 'd3d')
        part(several / 'frames-d3d-1', 'd3d')
        part(several / 'frames-gl-0', 'gl')
        self.assertEqual(find_parts(single), {'vulkan': [single]})
        got = find_parts(several)
        self.assertEqual({r: len(p) for r, p in got.items()}, {'d3d': 2, 'gl': 1})

    def test_merging_shards(self):
        ghosts = ['RR_A-s-p', 'RR_B-s-p', 'RR_C-s-p']
        parts = []
        for i, group in enumerate([ghosts[:2], ghosts[2:]]):
            part = self.root / f'frames-test-{i}'
            (part / 'frames').mkdir(parents=True)
            for g in group:
                Image.new('RGB', (4, 4)).save(part / 'frames' / f'{g.split("-")[0]}_00070.png')
            (part / 'manifest.json').write_text(json.dumps(dict(
                renderer='test', commit='c', environment={'image': str(i)}, size=[4, 4], maps=group, failures=[],
                runs=[dict(ghost=g, tics=[70], seconds=1, exit=0) for g in group], seconds=2)))
            parts.append(part)
        import os
        cwd = os.getcwd()
        os.chdir(self.root)
        try:
            dest, m = merge('test', parts, 2, True, 3)
            self.assertTrue(m['full'])
            self.assertEqual(m['failures'], [])
            self.assertEqual(m['map_environments']['RR_C'], {'image': '1'})
            self.assertEqual(len(m['environment']['runner_environments']), 2)
            self.assertEqual(len(list((dest / 'frames').glob('*.png'))), 3)
            _, short = merge('test', parts[:1], 2, True, 3)
            self.assertFalse(short['full'])
            self.assertTrue(any('Missing shard' in f for f in short['failures']))
        finally:
            os.chdir(cwd)

    def test_a_renderer_can_add_its_own_errors(self):
        self.assertEqual(log_errors('vulkan', 'fine\nVUID-vkCmdDraw-x\n'), ['VUID-'])
        with patch.dict(RENDERERS['vulkan'], errors=['failed assertion']):
            self.assertEqual(log_errors('vulkan', 'a failed assertion happened'), ['failed assertion'])
        self.assertEqual(log_errors('vulkan', 'a failed assertion happened'), [])

    def test_engine_log_is_read_once(self):
        home = self.root / 'home'
        (home / '.ringracers').mkdir(parents=True)
        self.assertEqual(engine_log(home), '')
        (home / '.ringracers' / 'latest-log.txt').write_text('the whole log\n')
        (home / '.ringracers' / 'log.txt').write_text('short')
        (home / '.ringracers' / 'config.cfg').write_text('log lookalike')
        self.assertEqual(engine_log(home), 'the whole log\n')

    def test_logs(self):
        for line in ('I_Error', 'VUID-123', 'not implemented', 'D3D12 debug ERROR: x'):
            self.assertRegex(line, ERROR)


if __name__ == '__main__':
    unittest.main()
