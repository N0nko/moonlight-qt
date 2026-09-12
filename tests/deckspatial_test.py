#!/usr/bin/env python3
import array
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

ROOT = Path(__file__).resolve().parents[1] / 'contrib/deck-spatial'
spec = importlib.util.spec_from_file_location('spatial', ROOT / 'control.py')
spatial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(spatial)
TARGET = 'alsa_output.pci-0000_04_00.5-platform-nau8821-max.HiFi__Speaker__sink'


def node(name, kind='Audio/Sink', id=1, state='running'):
    return {'id': id, 'type': 'PipeWire:Interface:Node',
            'info': {'state': state, 'props': {'node.name': name, 'media.class': kind,
                                             'object.serial': id + 100}}}


def link(source, target, id, state='active'):
    return {'id': id, 'type': 'PipeWire:Interface:Link',
            'info': {'state': state, 'props': {'link.output.node': source, 'link.input.node': target}}}


def controls_node(width, distance):
    item = node(spatial.NAME)
    item['info']['params'] = {'Props': [{'params': ['safety:width', width / 100,
                                                  'safety:distance', distance / 100]}]}
    return item


class SpeakerTests(unittest.TestCase):
    def test_exact_speaker_only(self):
        self.assertEqual(spatial.speaker_target([node(TARGET), node('bluez_output.test'),
                                               node(TARGET.replace('Speaker', 'Headphones'))]), TARGET)
        for items in ([], [node('bluez_output.test')], [node(TARGET, 'Audio/Source')],
                      [node(TARGET), node(TARGET)]):
            with self.assertRaises(RuntimeError):
                spatial.speaker_target(items)

    def test_graph(self):
        for mode, count in ((1, 2), (2, 8)):
            g = spatial.graph(mode, ROOT, TARGET)
            args = g['context.modules'][-1]['args']
            self.assertEqual(args['capture.props']['filter.smart.target'], {'node.name': TARGET})
            self.assertFalse(args['capture.props']['filter.smart.targetable'])
            self.assertTrue(args['playback.props']['node.passive'])
            self.assertTrue(args['playback.props']['stream.dont-remix'])
            graph = args['filter.graph']
            self.assertEqual(len(graph['inputs']), count)
            self.assertEqual(graph['outputs'], ['safety:out_left', 'safety:out_right'])
            filters = {n['name']: n for n in graph['nodes']}
            self.assertEqual(len(filters), len(graph['nodes']))
            for link in graph['links']:
                for port in link.values():
                    self.assertIn(port.split(':')[0], filters)
            # Every FIR uses a unique matrix channel; all output traverses safety.
            self.assertEqual(sorted(n['config']['channel'] for n in filters.values()
                                    if n['name'].startswith('c')), list(range(count * 2)))
            self.assertEqual(sorted(n['config']['channel'] for n in filters.values()
                                    if n.get('label') == 'convolver' and n['name'].startswith('room')),
                             list(range(4)))
            self.assertEqual(filters['safety']['control'],
                             {'width': 1, 'distance': 0 if mode == 1 else 1})
            encoded = json.dumps(g)
            for forbidden in ('force-quantum', 'force-rate', 'default.audio', 'bluez'):
                self.assertNotIn(forbidden, encoded)
        with self.assertRaises(ValueError):
            spatial.graph(9, ROOT, TARGET)

    def test_assets_and_mono(self):
        report = json.loads((ROOT / 'assets/design.json').read_text())
        for name, count in (('stereo.wav', 4), ('surround.wav', 16),
                            ('stereo-room.wav', 4), ('surround-room.wav', 4)):
            path = ROOT / 'assets' / name
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), report['assets'][name])
            with wave.open(str(path), 'rb') as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (count, 4, 48000))
                pcm = array.array('i', wav.readframes(wav.getnframes()))
                if sys.byteorder != 'little':
                    pcm.byteswap()
            self.assertLess(max(abs(v) for v in pcm), 2147483647)
            if name == 'stereo.wav':
                gain = 10 ** (report['stereo_gain_db'] / 20)
                for i in range(len(pcm) // 4):
                    self.assertEqual(pcm[i * 4], pcm[i * 4 + 3])
                    self.assertEqual(pcm[i * 4 + 1], pcm[i * 4 + 2])
                    expected = gain * 2147483647 if i == 48 else 0
                    self.assertLess(abs(pcm[i * 4] + pcm[i * 4 + 1] - expected), 2)
            if name == 'stereo-room.wav':
                for i in range(len(pcm) // 4):
                    self.assertEqual(pcm[i * 4] + pcm[i * 4 + 1], 0)
                    self.assertEqual(pcm[i * 4 + 2] + pcm[i * 4 + 3], 0)

    def test_state_migration_and_bounds(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(spatial, 'STATE', Path(tmp)):
            statefile = Path(tmp) / 'state.json'
            statefile.write_text('{"mode":2}')
            self.assertEqual(spatial.read_state(), {'mode': 2, 'profiles': {
                '1': {'width': 100, 'distance': 0}, '2': {'width': 100, 'distance': 100}}})
            statefile.write_text(json.dumps({'mode': True, 'profiles': {
                '1': {'width': 120, 'distance': 40}, '2': {'width': 151, 'distance': -1}}}))
            state = spatial.read_state()
            self.assertEqual(state['mode'], 0)
            self.assertEqual(state['profiles']['1'], {'width': 120, 'distance': 40})
            self.assertEqual(state['profiles']['2'], spatial.defaults(2))
            statefile.write_text('[]')
            self.assertEqual(spatial.read_mode(), 0)

    def test_live_controls(self):
        self.assertEqual(spatial.live_controls(controls_node(115, 125)), {'width': 115, 'distance': 125})
        self.assertEqual(spatial.live_controls(node(spatial.NAME)), {})
        self.assertEqual(spatial.live_controls(controls_node(float('nan'), float('inf'))), {})

    def test_tuning_live_persistence_and_rollback(self):
        for fail in (False, True):
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp)
                (path / 'state.json').write_text('{"mode":2}')
                original = json.dumps(spatial.graph(2, ROOT, TARGET), indent=2)
                (path / 'filter.conf').write_text(original)
                with patch.object(spatial, 'STATE', path), patch.object(spatial, 'UNITFILE', path / 'unit'), \
                        patch.object(spatial, 'command') as run, patch.object(spatial, 'nodes', side_effect=[
                            [controls_node(100, 100)], [controls_node(100, 100) if fail else controls_node(120, 140)]]):
                    if fail:
                        with self.assertRaisesRegex(RuntimeError, 'not applied'):
                            spatial.set_tuning(2, 120, 140)
                        self.assertEqual((path / 'filter.conf').read_text(), original)
                        self.assertEqual(spatial.read_state()['profiles']['2'], spatial.defaults(2))
                    else:
                        spatial.set_tuning(2, 120, 140)
                        self.assertEqual(spatial.read_state()['profiles']['2'], {'width': 120, 'distance': 140})
                        self.assertEqual(spatial.read_state()['profiles']['1'], spatial.defaults(1))
                    for call in run.call_args_list:
                        self.assertEqual(call.args[:2], ('/usr/bin/pw-cli', 'set-param'))
                        self.assertIsInstance(json.loads(call.args[-1])['params'], list)
                    self.assertEqual(run.call_count, 2 if fail else 1)
                    with self.assertRaisesRegex(RuntimeError, 'mode changed'):
                        spatial.set_tuning(1, 100, 100)
                    with self.assertRaises(ValueError):
                        spatial.set_tuning(2, 151, 100)

    def test_routing_requires_links_even_if_failed_source_becomes_idle(self):
        items = [node(TARGET), node(spatial.NAME, id=2), node(spatial.NAME + '-output', id=3),
                 node('player', 'Stream/Output/Audio', id=4)]
        links = [link(4, 2, 10), link(3, 1, 11), link(3, 1, 12)]
        self.assertEqual(spatial.playing_sources(items + links, TARGET), {'104'})
        with patch.object(spatial, 'nodes', return_value=items + links):
            spatial.wait_routing({'104'}, timeout=0)
        items[-1]['info']['state'] = 'idle'
        with patch.object(spatial, 'nodes', return_value=items):
            with self.assertRaisesRegex(RuntimeError, 'links'):
                spatial.wait_routing({'104'}, timeout=0)
        for item in links:
            item['info']['state'] = 'paused'
        with patch.object(spatial, 'nodes', return_value=items + links):
            spatial.wait_routing({'104'}, timeout=0)
        with patch.object(spatial, 'nodes', return_value=items[:-1]):
            spatial.wait_routing({'104'}, timeout=0)  # Application really exited.

    def test_off_and_ownership(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            with patch.object(spatial, 'STATE', path), patch.object(spatial, 'UNITFILE', path / 'unit'), \
                    patch.object(spatial, 'command') as run:
                spatial.set_mode(0)
                run.assert_not_called()
                self.assertEqual(spatial.read_mode(), 0)
                (path / 'unit').write_text('unrelated service')
                with self.assertRaises(RuntimeError):
                    spatial.set_mode(0)
                self.assertEqual((path / 'unit').read_text(), 'unrelated service')

    def test_failed_activation_restores_previous_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            files = {path / 'unit': spatial.MARKER + 'previous unit',
                     path / 'filter.conf': 'previous graph', path / 'state.json': '{"mode": 1}'}
            for file, data in files.items():
                file.write_text(data)
            calls = []

            def run(*args, **kwargs):
                calls.append(args)
                if 'restart' in args:
                    raise RuntimeError('test load failure')
                return subprocess.CompletedProcess(args, 0, '', '')

            with patch.object(spatial, 'STATE', path), patch.object(spatial, 'UNITFILE', path / 'unit'), \
                    patch.object(spatial, 'command', side_effect=run), \
                    patch.object(spatial, 'status', return_value={'available': True}), \
                    patch.object(spatial, 'payload', return_value=ROOT), \
                    patch.object(spatial, 'wait_routing'), \
                    patch.object(spatial, 'nodes', return_value=[node(TARGET)]):
                with self.assertRaisesRegex(RuntimeError, 'test load failure'):
                    spatial.set_mode(2)
            for file, data in files.items():
                self.assertEqual(file.read_text(), data)
            self.assertTrue(any('start' in call for call in calls))

    def test_failed_links_and_failed_rollback_switch_processing_off(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'unit').write_text(spatial.MARKER + 'previous unit')
            (path / 'state.json').write_text('{"mode":1}')
            with patch.object(spatial, 'STATE', path), patch.object(spatial, 'UNITFILE', path / 'unit'), \
                    patch.object(spatial, 'command', return_value=subprocess.CompletedProcess([], 0)) as run, \
                    patch.object(spatial, 'status', return_value={'available': True}), \
                    patch.object(spatial, 'payload', return_value=ROOT), \
                    patch.object(spatial, 'nodes', return_value=[node(TARGET)]), \
                    patch.object(spatial, 'wait_routing', side_effect=RuntimeError('broken links')):
                with self.assertRaisesRegex(RuntimeError, 'switched Off'):
                    spatial.set_mode(2)
                self.assertEqual(spatial.read_mode(), 0)
                self.assertEqual(run.call_args.args[2:4], ('disable', '--now'))


if __name__ == '__main__':
    unittest.main()
