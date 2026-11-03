#!/usr/bin/env python3
"""Authentication and rule-persistence smoke test. Run ONLY against a disposable Compose stack.

Usage: python3 scripts/test-auth.py /path/to/test-compose.yml http://localhost:48080
The stack must expose its backend and have a MariaDB service named db. This test
sets the seeded admin password and inserts test records in that disposable DB.
"""

import base64
import hashlib
import json
import os
import secrets
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    stack_file, base = sys.argv[1:]
    compose = ['docker', 'compose', '-f', stack_file]
    password = secrets.token_urlsafe(24)

    def run(*args, input=None):
        return subprocess.run(compose + list(args), input=input, text=True,
                              capture_output=True, check=True).stdout

    def sql(statement):
        return run('exec', '-T', 'db', 'sh', '-c',
                   'mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" homestr_db', input=statement)

    def request(path, token=None, method='GET', body=None, expected=200):
        headers = {'Content-Type': 'application/json'}
        if token:
            headers['Authorization'] = 'Bearer ' + token
        req = urllib.request.Request(base + '/api/v1' + path, method=method,
                                     headers=headers,
                                     data=json.dumps(body).encode() if body is not None else None)
        try:
            response = urllib.request.urlopen(req, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            assert response.status == expected, (path, response.status, expected)
            data = response.read()
            return json.loads(data) if data else None

    def login(email='admin@homestr.local'):
        return request('/auth/login', method='POST', body={'email': email, 'password': password})['token']

    def rotate():
        run('exec', '-T', 'backend', '/app/build/homestr_server', '--set-admin-password',
            input=password + '\n' + password + '\n')

    class WebSocket:
        def __init__(self, token=None):
            from urllib.parse import urlparse
            url = urlparse(base)
            self.socket = socket.create_connection((url.hostname, url.port), timeout=5)
            self.stream = self.socket.makefile('rb')
            key = base64.b64encode(os.urandom(16)).decode()
            protocols = 'homestr' + (', bearer.' + token if token else '')
            self.socket.sendall((f'GET /api/v1/ws HTTP/1.1\r\nHost: {url.netloc}\r\n'
                                 'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                                 f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n'
                                 f'Sec-WebSocket-Protocol: {protocols}\r\n\r\n').encode())
            self.status = self.stream.readline()
            self.headers = {}
            if b'101' in self.status:
                while True:
                    line = self.stream.readline()
                    if line in (b'\r\n', b''):
                        break
                    name, value = line.decode().split(':', 1)
                    self.headers[name.lower()] = value.strip()

        def frame(self):
            header = self.stream.read(2)
            assert len(header) == 2, 'Connection closed without a frame'
            size = header[1] & 127
            if size == 126:
                size = struct.unpack('!H', self.stream.read(2))[0]
            elif size == 127:
                size = struct.unpack('!Q', self.stream.read(8))[0]
            return header[0] & 15, self.stream.read(size)

        def close(self):
            self.stream.close()
            self.socket.close()

    # Every protected route rejects missing or invented credentials.
    for path, method in [('/dashboard', 'GET'), ('/devices', 'GET'), ('/devices', 'POST'),
                         ('/devices/discover', 'GET'), ('/devices/unknown', 'GET'),
                         ('/devices/unknown', 'DELETE'), ('/devices/unknown/commands', 'POST'),
                         ('/rules', 'GET'), ('/rules', 'POST'), ('/rules/unknown/enabled', 'PUT'),
                         ('/rules/unknown/trigger', 'POST'), ('/auth/me', 'GET'),
                         ('/auth/logout', 'POST')]:
        request(path, method=method, expected=401)
    request('/dashboard', token='dev-token-admin', expected=401)
    denied = WebSocket()
    assert b'101' not in denied.status, 'Unauthenticated WebSocket was accepted'
    denied.close()
    request('/auth/login', method='POST', body={'email': 3, 'password': []}, expected=400)
    request('/auth/login', method='POST', body={'email': 'admin@homestr.local',
                                             'password': 'unconfigured-password'}, expected=401)
    rotate()
    request('/auth/login', method='POST', body={'email': 'admin@homestr.local',
                                             'password': 'incorrect-password'}, expected=401)
    admin = login()
    assert request('/auth/me', admin)['role'] == 'admin'
    assert 'password_hash' not in request('/auth/me', admin)
    sql("INSERT INTO users (id, name, email, password_hash, role) "
        "SELECT 'auth-test-member', 'Member', 'member@test.local', password_hash, 'user' "
        "FROM users WHERE email = 'admin@homestr.local' "
        "ON DUPLICATE KEY UPDATE password_hash = VALUES(password_hash);")
    member = login('member@test.local')
    request('/devices', member, 'POST', {}, expected=403)
    request('/devices/discover', member, expected=403)
    request('/devices/unknown', member, 'DELETE', expected=403)
    sql("DELETE FROM devices WHERE id = 'auth-test-device';")
    device = {'id': 'auth-test-device', 'name': 'Test light', 'ipAddress': '127.0.0.1',
              'householdId': '00000000-0000-0000-0000-0000000000a1',
              'type': 'light', 'status': 'ONLINE'}
    request('/devices', admin, 'POST', device, expected=201)
    request('/devices/auth-test-device/commands', member, 'POST', {'command': 'turnOn'})
    audit = sql("SELECT actor_type, actor_id FROM device_events WHERE device_id = 'auth-test-device';")
    assert 'USER\tauth-test-member' in audit
    # Same-device actions deliberately repeat to verify sequence, not just membership.
    rule = {
        'id': 'auth-test-rule', 'name': 'Bedtime: "kitchen"', 'enabled': True,
        'triggerType': 'MANUAL', 'trigger': {'note': "saved payload's value"},
        'actions': [
            {'deviceId': 'auth-test-device', 'command': 'turnOn', 'params': {}},
            {'deviceId': 'auth-test-device', 'command': 'setBrightness', 'params': {'level': 17}},
            {'deviceId': 'auth-test-device', 'command': 'turnOff', 'params': {}},
        ],
    }
    disabled_rule = dict(rule, id='auth-disabled-rule', enabled=False, triggerType='DEVICE_STATE',
                         trigger={'deviceId': 'auth-test-device', 'attribute': 'powerOn', 'equals': True})
    empty_rule = dict(rule, id='auth-empty-rule', triggerType='SCHEDULE',
                      trigger={'cron': '0 22 * * *'}, actions=[])
    sql("DELETE FROM automation_rules WHERE id IN ('auth-test-rule', 'auth-disabled-rule', 'auth-empty-rule');")
    for entry in [rule, disabled_rule, empty_rule]:
        assert request('/rules', admin, 'POST', entry, expected=201) == entry
    request('/rules', admin, 'POST', dict(rule, name='Duplicate'), expected=409)
    request('/rules/auth-test-rule/enabled', admin, 'PUT', {'enabled': 'false'}, expected=400)
    request('/rules/unknown/enabled', admin, 'PUT', {'enabled': False}, expected=404)
    request('/rules/auth-test-rule/enabled', admin, 'PUT', {'enabled': False})
    rule['enabled'] = False
    expected_rules = {entry['id']: entry for entry in [rule, disabled_rule, empty_rule]}

    digest = hashlib.sha256(admin.encode()).hexdigest()
    assert digest in sql('SELECT token_hash FROM sessions;')
    assert admin not in sql('SELECT token_hash FROM sessions;')

    # Browser subprotocol negotiation must never echo the credential.
    ws = WebSocket(admin)
    assert b'101' in ws.status
    assert ws.headers.get('sec-websocket-protocol') == 'homestr'
    opcode, payload = ws.frame()
    assert opcode == 1 and json.loads(payload)['type'] == 'snapshot'
    assert {entry['id']: entry for entry in json.loads(payload)['payload']['rules']} == expected_rules
    request('/auth/logout', admin, 'POST', expected=204)
    opcode, payload = ws.frame()
    assert opcode == 8 and struct.unpack('!H', payload[:2])[0] == 4001
    ws.close()
    request('/dashboard', admin, expected=401)
    request('/auth/me', member)  # Logout must not revoke someone else's session.

    admin = login()
    ws = WebSocket(admin)
    ws.frame()
    digest = hashlib.sha256(admin.encode()).hexdigest()
    sql(f"UPDATE sessions SET expires_at = DATE_SUB(NOW(), INTERVAL 1 SECOND) WHERE token_hash = '{digest}';")
    opcode, payload = ws.frame()
    assert opcode == 8 and struct.unpack('!H', payload[:2])[0] == 4001
    ws.close()
    request('/dashboard', admin, expected=401)

    # Persisted sessions and devices remain valid after a process restart.
    admin = login()
    run('restart', 'backend')
    for attempt in range(30):
        try:
            request('/auth/me', admin)
            break
        except (OSError, AssertionError):
            time.sleep(0.25)
    else:
        raise AssertionError('Backend did not recover after restart')
    assert request('/devices/auth-test-device', admin)['powerOn']
    assert {entry['id']: entry for entry in request('/rules', admin)['rules']} == expected_rules
    assert {entry['id']: entry for entry in request('/dashboard', admin)['rules']} == expected_rules
    request('/rules/auth-test-rule/trigger', admin, 'POST', expected=404)
    assert request('/devices/auth-test-device', admin)['powerOn']
    request('/rules/auth-test-rule/enabled', admin, 'PUT', {'enabled': True})
    rule['enabled'] = True
    run('restart', 'backend')
    for attempt in range(30):
        try:
            request('/auth/me', admin)
            break
        except (OSError, AssertionError):
            time.sleep(0.25)
    else:
        raise AssertionError('Backend did not recover after second restart')
    assert {entry['id']: entry for entry in request('/rules', admin)['rules']} == expected_rules
    request('/rules/auth-test-rule/trigger', admin, 'POST')
    state = request('/devices/auth-test-device', admin)
    assert not state['powerOn'] and state['brightness'] == 17

    logs = run('logs', '--no-color', 'backend')
    assert '1 device(s) loaded' in logs
    assert '[db]' not in logs, 'Database errors in backend logs'
    assert admin not in logs and password not in logs
    # A failed action insert must roll back both the rule row and all preceding actions.
    bad_rule = dict(rule, id='auth-bad-rule', actions=rule['actions'] + [
        {'deviceId': 'missing-device', 'command': 'turnOn', 'params': {}},
    ])
    request('/rules', admin, 'POST', bad_rule, expected=503)
    assert 'auth-bad-rule' not in {entry['id'] for entry in request('/rules', admin)['rules']}
    assert sql("SELECT COUNT(*) AS n FROM automation_rules WHERE id = 'auth-bad-rule';").strip().endswith('0')
    assert sql("SELECT COUNT(*) AS n FROM automation_actions WHERE rule_id = 'auth-bad-rule';").strip().endswith('0')
    sql("DROP TRIGGER IF EXISTS auth_reject_actions; "
        "CREATE TRIGGER auth_reject_actions BEFORE INSERT ON automation_actions FOR EACH ROW "
        "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'test write failure';")
    try:
        request('/rules/auth-test-rule/enabled', admin, 'PUT', {'enabled': False}, expected=503)
        assert {entry['id']: entry for entry in request('/rules', admin)['rules']} == expected_rules
        assert sql("SELECT enabled FROM automation_rules WHERE id = 'auth-test-rule';").strip().endswith('1')
        assert sql("SELECT COUNT(*) AS n FROM automation_actions WHERE rule_id = 'auth-test-rule';").strip().endswith('3')
    finally:
        sql('DROP TRIGGER auth_reject_actions;')
    rotate()
    request('/auth/me', admin, expected=401)
    admin = login()
    request('/devices/auth-test-device', admin, 'DELETE', expected=204)
    sql("DELETE FROM automation_rules WHERE id IN ('auth-test-rule', 'auth-disabled-rule', 'auth-empty-rule');")
    print('PASS: ordered rule persistence, enabled-state recovery, atomic rollback, login, route guards, roles, audit attribution, token hashing, WebSocket auth, '
          'logout, expiry, restart persistence, and password rotation')


if __name__ == '__main__':
    main()
