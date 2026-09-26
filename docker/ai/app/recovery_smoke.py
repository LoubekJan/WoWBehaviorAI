"""Read-only live-model probe; proposes a token without touching the game."""
import json
import urllib.error
import urllib.request

def main():
    base = 'http://127.0.0.1:8000'
    with urllib.request.urlopen(base + '/health', timeout=5) as response:
        health = json.load(response)
    if not health.get('recovery_model_enabled') or not health.get('recovery_model_configured'):
        raise SystemExit('Recovery AI is disabled or missing its model URL/name. Set AI_RECOVERY_MODEL_* or reuse AI_TASK_MODEL_URL/NAME.')
    request = {'protocol_version': 1, 'request_id': 1, 'agent_id': 1, 'episode': 1,
        'role': 'GUARD', 'problem': 'RETURN_HOME', 'failure': 'PROBE', 'hunger': 0.0,
        'home_distance': 20.0, 'stalled_ms': 30000, 'failures': 3,
        'options': [{'token': 1, 'strategy': 'CORRIDOR', 'dx': 5.0, 'dy': 0.0, 'dz': 0.0,
            'distance': 5.0, 'home_gain': 5.0, 'nearby_prey': 0, 'visits': 0, 'successes': 0}]}
    try:
        with urllib.request.urlopen(urllib.request.Request(base + '/recovery',
                data=json.dumps(request).encode(), headers={'Content-Type': 'application/json'}), timeout=12) as response:
            answer = json.load(response)
    except (urllib.error.URLError, TimeoutError) as exc:
        raise SystemExit(f'Recovery AI probe failed: {exc}') from exc
    expected = {'protocol_version': 1, 'request_id': 1, 'agent_id': 1, 'episode': 1}
    if set(answer) != set(expected) | {'choice'} or any(answer[k] != v for k,v in expected.items()) or type(answer['choice']) is not int or answer['choice'] not in (0,1):
        raise SystemExit('Recovery AI probe returned an invalid response')
    print(f"Recovery AI: live model replied, choice={answer['choice']}. No game action was executed.")

if __name__ == '__main__':
    main()
