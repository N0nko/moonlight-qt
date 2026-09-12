#!/usr/bin/env python3
"""User-owned speaker smart filter. No default-device changes or resident watcher."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

NAME = 'moonlight-deck-spatial'
UNIT = NAME + '.service'
MARKER = '# Managed by Moonlight deck-spatial v1\n'
ROOT = Path(__file__).resolve().parent
STATE = Path.home() / '.local/state/moonlight/deck-spatial'
UNITFILE = Path.home() / '.config/systemd/user' / UNIT
FILES = ('assets/stereo.wav', 'assets/surround.wav', 'assets/design.json',
         'safety.lv2/manifest.ttl', 'safety.lv2/safety.ttl', 'safety.lv2/safety.so',
         'NOTICE')


def command(*args, check=True):
    result = subprocess.run(args, capture_output=True, text=True, timeout=8)
    if check and result.returncode:
        raise RuntimeError((result.stderr or result.stdout or 'Command failed').strip()[:400])
    return result


def nodes():
    return json.loads(command('/usr/bin/pw-dump').stdout)


def properties(node):
    return node.get('info', {}).get('props', {})


def speaker_target(items):
    targets = [properties(n)['node.name'] for n in items
               if properties(n).get('media.class') == 'Audio/Sink'
               and properties(n).get('node.name', '').startswith('alsa_output.')
               and 'nau8821-max.' in properties(n).get('node.name', '')
               and properties(n).get('node.name', '').endswith('__Speaker__sink')]
    if len(targets) != 1:
        raise RuntimeError('Built-in OLED speakers not found; audio left unchanged')
    return targets[0]


def graph(mode, payload, target):
    if mode not in (1, 2):
        raise ValueError('Unknown speaker mode')
    channels = ['FL', 'FR'] if mode == 1 else ['FL', 'FR', 'FC', 'LFE', 'RL', 'RR', 'SL', 'SR']
    count = len(channels)
    wav = str(payload / 'assets' / ('stereo.wav' if mode == 1 else 'surround.wav'))
    filters, links = [], []
    for i in range(count):
        filters.append({'type': 'builtin', 'name': f'in{i}', 'label': 'copy'})
        for ear in range(2):
            name = f'c{ear}_{i}'
            filters.append({'type': 'builtin', 'name': name, 'label': 'convolver',
                            'config': {'filename': wav, 'channel': ear * count + i,
                                       'length': 512 if mode == 1 else (2047 if i in (4, 5) else 1023),
                                       'blocksize': 64, 'tailsize': 256,
                                       'latency': .001 if mode == 1 else .002}})
            links.extend([{'output': f'in{i}:Out', 'input': name + ':In'},
                          {'output': name + ':Out', 'input': f'mix{ear}:In {i + 1}'}])
    for ear in range(2):
        filters.append({'type': 'builtin', 'name': f'mix{ear}', 'label': 'mixer',
                        'control': {f'Gain {i + 1}': 1 for i in range(count)}})
        links.append({'output': f'mix{ear}:Out',
                      'input': 'safety:' + ('left' if ear == 0 else 'right')})
    filters.append({'type': 'lv2', 'name': 'safety',
                    'plugin': 'urn:moonlight:deck-speaker-safety'})
    return {
        'context.properties': {'log.level': 2},
        'context.spa-libs': {'audio.convert.*': 'audioconvert/libspa-audioconvert',
                             'support.*': 'support/libspa-support'},
        'context.modules': [
            {'name': 'libpipewire-module-rt', 'flags': ['ifexists', 'nofail']},
            {'name': 'libpipewire-module-protocol-native'},
            {'name': 'libpipewire-module-client-node'},
            {'name': 'libpipewire-module-adapter'},
            {'name': 'libpipewire-module-filter-chain', 'args': {
                'node.description': 'Deck speaker spatial',
                'audio.rate': 48000,
                'filter.graph': {'nodes': filters, 'links': links,
                                 'inputs': [f'in{i}:In' for i in range(count)],
                                 'outputs': ['safety:out_left', 'safety:out_right']},
                'capture.props': {
                    'node.name': NAME, 'media.class': 'Audio/Sink',
                    'audio.position': channels, 'channelmix.upmix': False,
                    'node.virtual': True, 'node.passive': True, 'priority.session': 0,
                    'filter.smart': True, 'filter.smart.name': NAME,
                    'filter.smart.targetable': False,
                    'filter.smart.target': {'node.name': target}},
                'playback.props': {
                    'node.name': NAME + '-output', 'audio.position': ['FL', 'FR'],
                    'node.passive': True, 'stream.dont-remix': True}}}]}


def atomic(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + '.next')
    temp.write_text(content)
    os.replace(temp, path)


def owned_unit():
    if UNITFILE.exists() and not UNITFILE.read_text().startswith(MARKER):
        raise RuntimeError('Existing service is not owned by this helper')


def payload():
    report = json.loads((ROOT / 'assets/design.json').read_text())
    if set(report['assets']) != {'stereo.wav', 'surround.wav'}:
        raise RuntimeError('Incomplete speaker filter manifest')
    for name, digest in report['assets'].items():
        if name not in ('stereo.wav', 'surround.wav'):
            raise RuntimeError('Invalid asset manifest')
        if hashlib.sha256((ROOT / 'assets' / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('Speaker filter checksum failed')
    digest = hashlib.sha256()
    digest.update(b'payload-layout-v2')
    for name in FILES:
        digest.update(name.encode())
        digest.update((ROOT / name).read_bytes())
    dest = Path.home() / '.local/share/moonlight/deck-spatial' / digest.hexdigest()[:16]
    for name in FILES:
        out = dest / ('plugins/' + name if name.startswith('safety.lv2/') else name)
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists() and out.read_bytes() != (ROOT / name).read_bytes():
            raise RuntimeError('Installed speaker payload was modified')
        if not out.exists():
            shutil.copyfile(ROOT / name, out)
    return dest


def unit_text(data):
    def quote(value):
        return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%').replace('$', '$$') + '"'
    return MARKER + f'''[Unit]
Description=Moonlight speaker spatial audio
After=pipewire.service wireplumber.service
Requires=pipewire.service
PartOf=pipewire.service
StartLimitIntervalSec=30
StartLimitBurst=3

[Service]
ExecStart=/usr/bin/pipewire -c {quote(STATE / 'filter.conf')}
Environment={quote('LV2_PATH=' + str(data / 'plugins'))}
UnsetEnvironment=LD_LIBRARY_PATH LD_PRELOAD PIPEWIRE_CONFIG_DIR PIPEWIRE_CONFIG_NAME
Restart=on-failure
RestartSec=2
TimeoutStopSec=3

[Install]
WantedBy=default.target
'''


def read_mode():
    try:
        value = json.loads((STATE / 'state.json').read_text())['mode']
        return value if value in (0, 1, 2) else 0
    except (OSError, ValueError, KeyError):
        return 0


def status():
    mode = read_mode()
    result = {'mode': mode, 'active': False, 'available': False, 'message': ''}
    try:
        if Path('/sys/class/dmi/id/product_name').read_text().strip() != 'Galileo':
            raise RuntimeError('Speaker processing supports Steam Deck OLED only')
        if not all((ROOT / name).is_file() for name in FILES):
            raise RuntimeError('Speaker processing package is incomplete')
        items = nodes()
        speaker_target(items)
        result['available'] = True
        result['active'] = any(properties(n).get('node.name') == NAME for n in items)
        result['message'] = ('Speaker filter ready; headphones and Bluetooth bypass it'
                             if result['active'] else
                             'Off; original speaker audio' if mode == 0 else
                             'Filter stopped; select a mode to retry. Original audio is available')
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result['message'] = str(error)
    return result


def set_mode(mode):
    owned_unit()
    if mode == 0:
        if UNITFILE.exists():
            command('/usr/bin/systemctl', '--user', 'disable', '--now', UNIT)
        atomic(STATE / 'state.json', json.dumps({'mode': 0}))
        return
    if not status()['available']:
        raise RuntimeError(status()['message'])
    data = payload()
    config = json.dumps(graph(mode, data, speaker_target(nodes())), indent=2)
    old = {p: p.read_text() if p.exists() else None
           for p in (UNITFILE, STATE / 'filter.conf', STATE / 'state.json')}
    was_active = command('/usr/bin/systemctl', '--user', 'is-active', '--quiet', UNIT, check=False).returncode == 0
    was_enabled = command('/usr/bin/systemctl', '--user', 'is-enabled', '--quiet', UNIT, check=False).returncode == 0
    if (was_active and was_enabled and read_mode() == mode
            and old[UNITFILE] == unit_text(data) and old[STATE / 'filter.conf'] == config
            and any(properties(n).get('node.name') == NAME for n in nodes())):
        return
    try:
        atomic(STATE / 'filter.conf', config)
        atomic(UNITFILE, unit_text(data))
        command('/usr/bin/systemctl', '--user', 'daemon-reload')
        command('/usr/bin/systemctl', '--user', 'reset-failed', UNIT, check=False)
        command('/usr/bin/systemctl', '--user', 'restart', UNIT)
        # A bounded activation check only; no timer or poller remains running.
        deadline = time.monotonic() + 3
        while not any(properties(n).get('node.name') == NAME for n in nodes()):
            if time.monotonic() >= deadline:
                raise RuntimeError('Speaker graph did not start; previous mode restored')
            time.sleep(.05)
        command('/usr/bin/systemctl', '--user', 'enable', UNIT)
        atomic(STATE / 'state.json', json.dumps({'mode': mode}))
    except Exception:
        command('/usr/bin/systemctl', '--user', 'disable', '--now', UNIT, check=False)
        for path, content in old.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                atomic(path, content)
        command('/usr/bin/systemctl', '--user', 'daemon-reload', check=False)
        if was_enabled:
            command('/usr/bin/systemctl', '--user', 'enable', UNIT, check=False)
        if was_active:
            command('/usr/bin/systemctl', '--user', 'start', UNIT, check=False)
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['status', 'mode'])
    parser.add_argument('value', type=int, choices=[0, 1, 2], nargs='?')
    args = parser.parse_args()
    if args.action == 'mode' and args.value is None:
        parser.error('mode requires 0, 1 or 2')
    try:
        if args.action == 'mode':
            import fcntl
            STATE.mkdir(parents=True, exist_ok=True)
            with (STATE / 'lock').open('w') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                set_mode(args.value)
        print(json.dumps(status()))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result = status()
        result['message'] = str(error)
        print(json.dumps(result))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
