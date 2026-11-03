#!/usr/bin/env python3
"""Automatic trigger smoke test. ONLY use a disposable Compose stack.

Usage: python3 scripts/test-triggers.py docker-compose.auth-test.yml http://localhost:48080
Sets the test admin password and creates devices/rules. Waits up to one minute
for a real schedule tick, then checks durable claims across a backend restart.
Run after test-auth.py in the same disposable stack, or against a fresh stack.
"""
import json
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    stack, base = sys.argv[1:]
    compose = ['docker', 'compose', '-f', stack]
    password = secrets.token_urlsafe(24)

    def run(*args, input=None):
        return subprocess.run(compose + list(args), input=input, text=True,
                              capture_output=True, check=True).stdout

    def sql(statement):
        return run('exec', '-T', 'db', 'sh', '-c',
                   'mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" homestr_db', input=statement)

    run('exec', '-T', 'backend', '/app/build/homestr_server', '--set-admin-password',
        input=password + '\n' + password + '\n')
    token = None

    def request(path, body=None, method=None, expected=200):
        headers = {'Content-Type': 'application/json'}
        if token:
            headers['Authorization'] = 'Bearer ' + token
        req = urllib.request.Request(base + '/api/v1' + path, headers=headers,
                                     method=method or ('POST' if body is not None else 'GET'),
                                     data=json.dumps(body).encode() if body is not None else None)
        try:
            response = urllib.request.urlopen(req, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            assert response.status == expected, (path, response.status, expected)
            data = response.read()
            return json.loads(data) if data else None

    token = request('/auth/login', {'email': 'admin@homestr.local', 'password': password})['token']

    def restart():
        run('restart', 'backend')
        for _ in range(100):
            try:
                request('/auth/me')
                return
            except (OSError, AssertionError):
                time.sleep(0.1)
        raise AssertionError('Backend did not restart')

    # Clean only this script's fixtures; the test stack is explicitly disposable.
    sql("DELETE FROM automation_rules WHERE id LIKE 'trigger-test-%';")
    restart()
    for name in ['a', 'b']:
        device_id = 'trigger-test-' + name
        devices = request('/devices')['devices']
        if any(device['id'] == device_id for device in devices):
            request('/devices/' + device_id, method='DELETE', expected=204)
        request('/devices', {'id': device_id, 'householdId': '00000000-0000-0000-0000-0000000000a1',
                            'name': name, 'ipAddress': '127.0.0.1', 'type': 'light', 'status': 'ONLINE'}, expected=201)

    def command(device, command, params=None):
        return request('/devices/trigger-test-' + device + '/commands', {'command': command, 'params': params or {}})

    def state(device):
        return request('/devices/trigger-test-' + device)

    def enable(rule, enabled):
        return request('/rules/trigger-test-' + rule + '/enabled', {'enabled': enabled}, method='PUT')

    def state_rule(name, expected, target, command):
        return {'id': 'trigger-test-' + name, 'name': name, 'enabled': True, 'triggerType': 'DEVICE_STATE',
                'trigger': {'deviceId': 'trigger-test-a', 'attribute': 'powerOn', 'equals': expected},
                'actions': [{'deviceId': 'trigger-test-' + target, 'command': command, 'params': {}}]}

    for trigger_type, trigger in [
        ('SCHEDULE', {'cron': '256 * * * *'}),
        ('SCHEDULE', {'cron': '*/0 * * * *'}),
        ('SCHEDULE', {'cron': '* * * * *', 'timezone': 'Missing/Zone'}),
        ('DEVICE_STATE', {'deviceId': 'missing', 'attribute': 'powerOn', 'equals': True}),
        ('DEVICE_STATE', {'deviceId': 'trigger-test-a', 'attribute': 'powerOn', 'equals': 'true'}),
        ('DEVICE_STATE', {'deviceId': 'trigger-test-a', 'attribute': 'volume', 'equals': 10}),
    ]:
        request('/rules', {'id': 'trigger-test-invalid', 'name': 'Invalid', 'triggerType': trigger_type,
                           'trigger': trigger, 'actions': []}, expected=400)

    request('/rules', state_rule('follow', True, 'b', 'turnOn'), expected=201)
    command('a', 'turnOn')
    assert state('b')['powerOn']
    assert 'RULE\ttrigger-test-follow' in sql("SELECT actor_type, actor_id FROM device_events WHERE actor_id = 'trigger-test-follow';")
    command('b', 'turnOff')
    command('a', 'turnOn')
    command('a', 'setBrightness', {'level': 15})
    assert not state('b')['powerOn'], 'No-op or unrelated update retriggered the rule'
    enable('follow', False)
    command('a', 'turnOff')
    command('a', 'turnOn')
    assert not state('b')['powerOn']
    enable('follow', True)
    restart()
    assert not state('b')['powerOn'], 'Startup hydration fired a condition'
    command('a', 'turnOff')
    command('a', 'turnOn')
    assert state('b')['powerOn'], 'Restored state rule did not fire'
    enable('follow', False)
    command('a', 'turnOff')
    request('/rules', state_rule('loop-on', True, 'a', 'turnOff'), expected=201)
    request('/rules', state_rule('loop-off', False, 'a', 'turnOn'), expected=201)
    before = int(sql("SELECT COUNT(*) FROM device_events WHERE device_id='trigger-test-a';").splitlines()[-1])
    command('a', 'turnOn')
    after = int(sql("SELECT COUNT(*) FROM device_events WHERE device_id='trigger-test-a';").splitlines()[-1])
    assert after - before == 3, 'Circular rule chain did not terminate as expected'
    enable('loop-on', False)
    enable('loop-off', False)

    command('b', 'turnOff')
    schedule = request('/rules', {'id': 'trigger-test-clock', 'name': 'Every minute', 'enabled': True,
        'triggerType': 'SCHEDULE', 'trigger': {'cron': '* * * * *', 'timezone': 'UTC'},
        'actions': [{'deviceId': 'trigger-test-b', 'command': 'turnOn', 'params': {}}]}, expected=201)
    deadline = datetime.fromisoformat(schedule['nextRunAt'].replace('Z', '+00:00')).timestamp()
    print('Waiting for the next real minute boundary...', flush=True)
    while time.time() < deadline + 8:
        if state('b')['powerOn']:
            break
        time.sleep(0.2)
    else:
        raise AssertionError('Scheduled routine did not run automatically')
    claim = int(sql("SELECT UNIX_TIMESTAMP(last_fired_at) FROM automation_rules WHERE id='trigger-test-clock';").splitlines()[-1])
    assert claim == int(deadline)
    restart()
    command('b', 'turnOff')
    time.sleep(1.2)
    assert not state('b')['powerOn'], 'Schedule fired twice in the same minute after restart'
    count = int(sql("SELECT COUNT(*) FROM device_events WHERE actor_id='trigger-test-clock';").splitlines()[-1])
    assert count == 1
    paused = enable('clock', False)
    assert not paused['enabled']
    saved = {rule['id']: rule for rule in request('/rules')['rules']}
    assert saved['trigger-test-clock']['nextRunAt'] is None
    request('/auth/logout', method='POST', expected=204)
    print('PASS: trigger validation, transition-only rules, no-op suppression, RULE audit, '
          'restart recovery, loop guard, real scheduler tick, and durable schedule claim')


if __name__ == '__main__':
    main()
