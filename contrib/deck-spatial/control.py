#!/usr/bin/env python3
"""User-owned speaker smart filter. No default-device changes or resident watcher."""
import argparse
import hashlib
import json
import math
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
         'assets/stereo-room.wav', 'assets/surround-room.wav',
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


def defaults(mode):
    return {'width': 100, 'distance': 0 if mode == 1 else 100}


def graph(mode, payload, target, tuning=None):
    if mode not in (1, 2):
        raise ValueError('Unknown speaker mode')
    channels = ['FL', 'FR'] if mode == 1 else ['FL', 'FR', 'FC', 'LFE', 'RL', 'RR', 'SL', 'SR']
    count = len(channels)
    tuning = defaults(mode) if tuning is None else tuning
    wav = str(payload / 'assets' / ('stereo.wav' if mode == 1 else 'surround.wav'))
    filters, links = [], []
    for i in range(count):
        filters.append({'type': 'builtin', 'name': f'in{i}', 'label': 'copy'})
        for ear in range(2):
            name = f'c{ear}_{i}'
            filters.append({'type': 'builtin', 'name': name, 'label': 'convolver',
                            'config': {'filename': wav, 'channel': ear * count + i,
                                       'length': 512 if mode == 1 else 1023,
                                       'blocksize': 64, 'tailsize': 256,
                                       'latency': .001 if mode == 1 else .002}})
            links.extend([{'output': f'in{i}:Out', 'input': name + ':In'},
                          {'output': name + ':Out', 'input': f'mix{ear}:In {i + 1}'}])
    for ear in range(2):
        filters.append({'type': 'builtin', 'name': f'mix{ear}', 'label': 'mixer',
                        'control': {f'Gain {i + 1}': 1 for i in range(count)}})
        links.append({'output': f'mix{ear}:Out',
                      'input': 'safety:' + ('left' if ear == 0 else 'right')})
        filters.append({'type': 'builtin', 'name': f'roommix{ear}', 'label': 'mixer',
                        'control': {'Gain 1': 1, 'Gain 2': 1}})
        for j, channel in enumerate((0, 1) if mode == 1 else (4, 5)):
            room_name = f'room{ear}_{j}'
            filters.append({'type': 'builtin', 'name': room_name, 'label': 'convolver',
                            'config': {'filename': str(payload / 'assets' /
                                        ('stereo-room.wav' if mode == 1 else 'surround-room.wav')),
                                       'channel': ear * 2 + j, 'blocksize': 64, 'tailsize': 256,
                                       'latency': .001 if mode == 1 else .002}})
            links.extend([{'output': f'in{channel}:Out', 'input': room_name + ':In'},
                          {'output': room_name + ':Out', 'input': f'roommix{ear}:In {j + 1}'}])
        links.append({'output': f'roommix{ear}:Out',
                      'input': 'safety:' + ('room_left' if ear == 0 else 'room_right')})
    filters.append({'type': 'lv2', 'name': 'safety',
                    'plugin': 'urn:moonlight:deck-speaker-safety',
                    'control': {key: value / 100 for key, value in tuning.items()}})
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
    asset_names = {'stereo.wav', 'surround.wav', 'stereo-room.wav', 'surround-room.wav'}
    if set(report['assets']) != asset_names:
        raise RuntimeError('Incomplete speaker filter manifest')
    for name, digest in report['assets'].items():
        if name not in asset_names:
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


def read_state():
    state = {'mode': 0, 'profiles': {str(mode): defaults(mode) for mode in (1, 2)}}
    try:
        saved = json.loads((STATE / 'state.json').read_text())
        if type(saved.get('mode')) is int and saved['mode'] in (0, 1, 2):
            state['mode'] = saved['mode']
        for mode in ('1', '2'):
            for key, low, high in (('width', 50, 150), ('distance', 0, 200)):
                value = saved.get('profiles', {}).get(mode, {}).get(key)
                if type(value) is int and low <= value <= high:
                    state['profiles'][mode][key] = value
    except (OSError, ValueError, AttributeError, TypeError):
        pass
    return state


def read_mode():
    return read_state()['mode']


def live_controls(node):
    controls = {}
    for props in node.get('info', {}).get('params', {}).get('Props', []):
        values = props.get('params', [])
        controls.update(zip(values[::2], values[1::2]) if isinstance(values, list) else values)
    return {key: round(100 * controls['safety:' + key]) for key in ('width', 'distance')
            if isinstance(controls.get('safety:' + key), (int, float))
            and math.isfinite(controls['safety:' + key])}


def playing_sources(items, target):
    destinations = {n['id'] for n in items if properties(n).get('node.name') in (target, NAME)}
    linked = {int(properties(n)['link.output.node']) for n in items
              if int(properties(n).get('link.input.node', -1)) in destinations}
    return {str(properties(n).get('object.serial')) for n in items
            if n['id'] in linked and n.get('info', {}).get('state') == 'running'
            and properties(n).get('node.name') != NAME + '-output'}


def wait_routing(expected_sources, timeout=3):
    # A published sink alone is not proof of audio: WirePlumber can fail to link it.
    deadline = time.monotonic() + timeout
    while True:
        items = nodes()
        ids = {properties(n).get('node.name'): n['id'] for n in items
               if n.get('type', '').endswith(':Node')}
        target = speaker_target(items)
        present = NAME in ids and NAME + '-output' in ids
        # A failed link can make the source idle, so do not forget it on that transition.
        sources = {n['id'] for n in items if str(properties(n).get('object.serial')) in expected_sources}
        playing = {n['id'] for n in items if n['id'] in sources
                   and n.get('info', {}).get('state') == 'running'}
        connected = [(int(properties(n).get('link.output.node', -1)),
                      int(properties(n).get('link.input.node', -1))) for n in items
                     if n.get('info', {}).get('state') in ('active', 'paused')]
        active = [(int(properties(n).get('link.output.node', -1)),
                   int(properties(n).get('link.input.node', -1))) for n in items
                  if n.get('info', {}).get('state') == 'active']
        if present and (not sources or
                        (connected.count((ids[NAME + '-output'], ids[target])) >= 2
                         and all((source, ids[NAME]) in connected for source in sources)
                         and (not playing or
                              (active.count((ids[NAME + '-output'], ids[target])) >= 2
                               and all((source, ids[NAME]) in active for source in playing))))):
            return
        if time.monotonic() >= deadline:
            raise RuntimeError('Speaker playback links did not activate')
        time.sleep(.05)


def status():
    state = read_state()
    mode = state['mode']
    result = {'mode': mode, 'active': False, 'available': False, 'message': ''}
    result.update(state['profiles'].get(str(mode), defaults(2)))
    result['tunable'] = False
    try:
        if Path('/sys/class/dmi/id/product_name').read_text().strip() != 'Galileo':
            raise RuntimeError('Speaker processing supports Steam Deck OLED only')
        if not all((ROOT / name).is_file() for name in FILES):
            raise RuntimeError('Speaker processing package is incomplete')
        items = nodes()
        speaker_target(items)
        result['available'] = True
        result['active'] = any(properties(n).get('node.name') == NAME for n in items)
        for node in items:
            if properties(node).get('node.name') == NAME:
                actual = live_controls(node)
                result['tunable'] = len(actual) == 2
                result.update(actual)
        result['message'] = ('Speaker filter ready; headphones and Bluetooth bypass it'
                             if result['active'] else
                             'Off; original speaker audio' if mode == 0 else
                             'Filter stopped; select a mode to retry. Original audio is available')
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result['message'] = str(error)
    return result


def set_mode(mode):
    owned_unit()
    state = read_state()
    state['mode'] = mode
    if mode == 0:
        if UNITFILE.exists():
            command('/usr/bin/systemctl', '--user', 'disable', '--now', UNIT)
        atomic(STATE / 'state.json', json.dumps(state))
        return
    if not status()['available']:
        raise RuntimeError(status()['message'])
    data = payload()
    items = nodes()
    target = speaker_target(items)
    expected_sources = playing_sources(items, target)
    config = json.dumps(graph(mode, data, target, state['profiles'][str(mode)]), indent=2)
    old = {p: p.read_text() if p.exists() else None
           for p in (UNITFILE, STATE / 'filter.conf', STATE / 'state.json')}
    was_active = command('/usr/bin/systemctl', '--user', 'is-active', '--quiet', UNIT, check=False).returncode == 0
    was_enabled = command('/usr/bin/systemctl', '--user', 'is-enabled', '--quiet', UNIT, check=False).returncode == 0
    if (was_active and was_enabled and read_mode() == mode
            and old[UNITFILE] == unit_text(data) and old[STATE / 'filter.conf'] == config
            and any(properties(n).get('node.name') == NAME for n in nodes())):
        try:
            wait_routing(expected_sources, timeout=0)
            return
        except RuntimeError:
            pass
    try:
        atomic(STATE / 'filter.conf', config)
        atomic(UNITFILE, unit_text(data))
        command('/usr/bin/systemctl', '--user', 'daemon-reload')
        command('/usr/bin/systemctl', '--user', 'reset-failed', UNIT, check=False)
        command('/usr/bin/systemctl', '--user', 'restart', UNIT)
        wait_routing(expected_sources)
        command('/usr/bin/systemctl', '--user', 'enable', UNIT)
        atomic(STATE / 'state.json', json.dumps(state))
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
            try:
                wait_routing(expected_sources)
            except (RuntimeError, subprocess.SubprocessError):
                command('/usr/bin/systemctl', '--user', 'disable', '--now', UNIT, check=False)
                state = read_state()
                state['mode'] = 0
                atomic(STATE / 'state.json', json.dumps(state))
                raise RuntimeError('Speaker links failed; processing switched Off to restore original audio')
        raise


def set_tuning(mode, width, distance):
    if mode not in (1, 2) or not 50 <= width <= 150 or not 0 <= distance <= 200:
        raise ValueError('Speaker tuning outside safe range')
    owned_unit()
    state = read_state()
    if state['mode'] != mode:
        raise RuntimeError('Speaker mode changed; refresh the controls')
    current = [n for n in nodes() if properties(n).get('node.name') == NAME]
    if len(current) != 1 or len(live_controls(current[0])) != 2:
        raise RuntimeError('Select a speaker mode once to load the new controls')
    previous = live_controls(current[0])
    wanted = {'width': width, 'distance': distance}
    config_path = STATE / 'filter.conf'
    old_config = config_path.read_text()
    config = json.loads(old_config)
    filters = config['context.modules'][-1]['args']['filter.graph']['nodes']
    safety = next(n for n in filters if n['name'] == 'safety')
    safety['control'] = {key: value / 100 for key, value in wanted.items()}

    def apply(values):
        command('/usr/bin/pw-cli', 'set-param', str(current[0]['id']), 'Props',
                json.dumps({'params': [entry for key, value in values.items()
                                       for entry in (f'safety:{key}', value / 100)]}))

    try:
        apply(wanted)
        # Read back actual plugin controls, not merely the CLI exit status.
        updated = [n for n in nodes() if n['id'] == current[0]['id']
                   and properties(n).get('node.name') == NAME]
        if len(updated) != 1 or live_controls(updated[0]) != wanted:
            raise RuntimeError('Speaker controls were not applied')
        atomic(config_path, json.dumps(config, indent=2))
        state['profiles'][str(mode)] = wanted
        atomic(STATE / 'state.json', json.dumps(state))
    except Exception:
        try:
            apply(previous)
        finally:
            atomic(config_path, old_config)
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['status', 'mode', 'tune'])
    parser.add_argument('value', type=int, choices=[0, 1, 2], nargs='?')
    parser.add_argument('width', type=int, nargs='?')
    parser.add_argument('distance', type=int, nargs='?')
    args = parser.parse_args()
    if args.action == 'mode' and args.value is None:
        parser.error('mode requires 0, 1 or 2')
    if args.action == 'tune' and (args.value not in (1, 2) or args.width is None or args.distance is None):
        parser.error('tune requires MODE WIDTH DISTANCE')
    try:
        if args.action in ('mode', 'tune'):
            import fcntl
            STATE.mkdir(parents=True, exist_ok=True)
            with (STATE / 'lock').open('w') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                if args.action == 'mode':
                    set_mode(args.value)
                else:
                    set_tuning(args.value, args.width, args.distance)
        print(json.dumps(status()))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result = status()
        result['message'] = str(error)
        print(json.dumps(result))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
