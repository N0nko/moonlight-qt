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


def node(name, kind='Audio/Sink'):
    return {'info': {'props': {'node.name': name, 'media.class': kind}}}


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
                                    if n.get('label') == 'convolver'), list(range(count * 2)))
            encoded = json.dumps(g)
            for forbidden in ('force-quantum', 'force-rate', 'default.audio', 'bluez'):
                self.assertNotIn(forbidden, encoded)
        with self.assertRaises(ValueError):
            spatial.graph(9, ROOT, TARGET)

    def test_assets_and_mono(self):
        report = json.loads((ROOT / 'assets/design.json').read_text())
        for name, count in (('stereo.wav', 4), ('surround.wav', 16)):
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
                    patch.object(spatial, 'nodes', return_value=[node(TARGET)]):
                with self.assertRaisesRegex(RuntimeError, 'test load failure'):
                    spatial.set_mode(2)
            for file, data in files.items():
                self.assertEqual(file.read_text(), data)
            self.assertTrue(any('start' in call for call in calls))


if __name__ == '__main__':
    unittest.main()
