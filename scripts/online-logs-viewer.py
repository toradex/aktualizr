#!/usr/bin/env python3
"""
Online Update Logs Viewer

Command-line tool to display installation logs captured by the
``OnlineLogsUploader`` during an over-the-air update and stored on the
Torizon Cloud (app.torizon.io).

It calls the same API documented at:

    https://app.torizon.io/api/undocs/#/Updates/getUpdatesDevicesDeviceidUpdateidInstallation-logs

The Device ID and Update ID can be supplied directly via ``--device-id``
and ``--update-id``, or extracted from a Torizon Cloud update details URL.

Authentication is done with a Bearer token. The token is resolved in this
order: ``--token`` argument, ``TORIZON_API_TOKEN`` environment variable, or
``--credentials <file>`` which performs an OAuth2 ``client_credentials`` call
to the token endpoint specified in the file (defaulting to
``https://kc.torizon.io/auth/realms/ota-users/protocol/openid-connect/token``).

Usage:
    # From a Torizon device URL
    ./online-logs-viewer.py 'https://app.torizon.io/devices/<device>/updates/<update>?...'

    # From explicit IDs
    ./online-logs-viewer.py --device-id <device> --update-id <update>

    # Filter by service
    ./online-logs-viewer.py <url> --service aktualizr.service

    # Raw JSON output
    ./online-logs-viewer.py <url> --json

    # Auto-fetch a token from a credentials JSON file
    ./online-logs-viewer.py <url> --credentials torion-cred.json
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone


API_BASE = "https://app.torizon.io/api/v2beta"
DEFAULT_LIMIT = 1000


# ANSI color codes for terminal output
class Colors:
    HEADER = '\033[95m'
    BLUE = '\033[94m'
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    DIM = '\033[2m'


def supports_color():
    """Check if the terminal supports color output."""
    if not hasattr(sys.stdout, 'isatty'):
        return False
    if not sys.stdout.isatty():
        return False
    if os.environ.get('NO_COLOR'):
        return False
    return True


def colorize(text, color):
    """Apply color to text if terminal supports it."""
    if supports_color():
        return f"{color}{text}{Colors.ENDC}"
    return text


# ---------------------------------------------------------------------------
# URL parsing
# ---------------------------------------------------------------------------

# Match the app.torizon.io devices URL, including any query string/fragment.
# Examples that match:
#   https://app.torizon.io/devices/<id>/updates/<id>
#   http://app.torizon.io/devices/<id>/updates/<id>?filter=foo
DEVICE_URL_RE = re.compile(
    r'^https?://app\.torizon\.io/devices/(?P<device>[^/?#]+)/updates/(?P<update>[^/?#]+)',
    re.IGNORECASE,
)

# UUIDs (8-4-4-4-12) and the longer ULID-style update IDs Torizon uses.
ID_RE = re.compile(
    r'^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}'
    r'|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{16}$'
)


def parse_ids_from_url(url):
    """Extract device and update IDs from a Torizon Cloud device URL.

    Returns a ``(device_id, update_id)`` tuple or ``None`` if the URL does not
    look like a Torizon devices URL.
    """
    match = DEVICE_URL_RE.match(url.strip())
    if not match:
        return None
    return match.group('device'), match.group('update')


DEFAULT_TOKEN_ENDPOINT = (
    "https://kc.torizon.io/auth/realms/ota-users/protocol/openid-connect/token"
)


def load_credentials(path):
    """Load OAuth2 client credentials from a JSON file.

    The file must contain ``clientId`` and ``clientSecret``. ``tokenEndpoint``
    is optional and falls back to ``DEFAULT_TOKEN_ENDPOINT``.
    """
    try:
        with open(path, 'r', encoding='utf-8') as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"Error: cannot read credentials from {path}: {e}", file=sys.stderr)
        sys.exit(2)

    if not isinstance(data, dict):
        print(f"Error: credentials file {path} is not a JSON object", file=sys.stderr)
        sys.exit(2)

    client_id = data.get('clientId') or data.get('client_id')
    client_secret = data.get('clientSecret') or data.get('client_secret')
    if not client_id or not client_secret:
        print(
            f"Error: credentials file {path} must contain 'clientId' and 'clientSecret'",
            file=sys.stderr,
        )
        sys.exit(2)

    token_endpoint = data.get('tokenEndpoint') or data.get('token_endpoint') \
        or DEFAULT_TOKEN_ENDPOINT

    return client_id, client_secret, token_endpoint


def fetch_token(client_id, client_secret, token_endpoint):
    """Fetch an OAuth2 access token via the ``client_credentials`` grant.

    Performs the equivalent of::

        curl -X POST -H "Content-Type: application/x-www-form-urlencoded" \\
            -d "grant_type=client_credentials&client_id=...&client_secret=..." \\
            <token_endpoint>

    Returns the ``access_token`` string. Raises ``ApiError`` on failure.
    """
    body = urllib.parse.urlencode({
        'grant_type': 'client_credentials',
        'client_id': client_id,
        'client_secret': client_secret,
    }).encode('utf-8')

    req = urllib.request.Request(token_endpoint, data=body, method='POST')
    req.add_header('Content-Type', 'application/x-www-form-urlencoded')
    req.add_header('Accept', 'application/json')
    req.add_header('User-Agent', 'aktualizr-online-logs-viewer/1.0')

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            raw = resp.read().decode('utf-8', errors='replace')
            status = getattr(resp, 'status', resp.getcode())
    except urllib.error.HTTPError as e:
        raw = ''
        try:
            raw = e.read().decode('utf-8', errors='replace')
        except Exception:
            pass
        raise ApiError(e.code, raw) from e
    except urllib.error.URLError as e:
        raise ApiError(0, str(e.reason), message=f"Connection error: {e.reason}") from e

    if status < 200 or status >= 300:
        raise ApiError(status, raw)

    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        raise ApiError(status, raw, message=f"Token endpoint returned non-JSON: {e}") from e

    token = data.get('access_token')
    if not token:
        raise ApiError(status, raw, message="Token endpoint did not return an access_token")
    return token


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

class ApiError(Exception):
    """Raised for non-2xx API responses."""

    def __init__(self, status, body, message=None):
        self.status = status
        self.body = body
        super().__init__(message or f"HTTP {status}: {body[:200] if body else '<empty>'}")


def http_get(url, token, params=None):
    """Perform an authenticated GET and return the parsed JSON body.

    Raises ``ApiError`` on non-2xx responses.
    """
    if params:
        # Preserve order while stripping None values.
        query = urllib.parse.urlencode(
            [(k, v) for k, v in params.items() if v is not None], doseq=True
        )
        url = f"{url}?{query}" if query else url

    req = urllib.request.Request(url, method='GET')
    req.add_header('Authorization', f'Bearer {token}')
    req.add_header('Accept', 'application/json')
    req.add_header('User-Agent', 'aktualizr-online-logs-viewer/1.0')

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            body = resp.read().decode('utf-8', errors='replace')
            status = getattr(resp, 'status', resp.getcode())
    except urllib.error.HTTPError as e:
        body = ''
        try:
            body = e.read().decode('utf-8', errors='replace')
        except Exception:
            pass
        raise ApiError(e.code, body) from e
    except urllib.error.URLError as e:
        raise ApiError(0, str(e.reason), message=f"Connection error: {e.reason}") from e

    if status < 200 or status >= 300:
        raise ApiError(status, body)

    if not body:
        return {}
    return json.loads(body)


def fetch_all_logs(device_id, update_id, token, service=None, page_size=DEFAULT_LIMIT):
    """Fetch all installation log messages, transparently paging through results.

    Returns a list of ``InstallLogMessage`` dicts.
    """
    url = f"{API_BASE}/updates/devices/{device_id}/{update_id}/installation-logs"
    logs = []
    offset = 0
    total = None

    while True:
        params = {'offset': offset, 'limit': page_size}
        if service:
            params['service'] = service

        data = http_get(url, token, params=params)
        values = data.get('values', []) or []
        logs.extend(values)

        total = data.get('total', len(logs))
        offset += len(values)

        # Stop when the server has no more rows, or the page came back short.
        if not values or offset >= total:
            break
        # Defensive stop to avoid runaway loops on a misbehaving server.
        if len(values) < page_size:
            break

    return logs, total


# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------

def format_timestamp(ts):
    """Format an ISO-8601 timestamp string as a human-readable UTC time.

    Falls back to the original string if parsing fails.
    """
    if not ts:
        return "N/A"
    try:
        # Tolerate trailing 'Z' (UTC) that ``fromisoformat`` rejects on older Pythons.
        normalized = ts.replace('Z', '+00:00')
        dt = datetime.fromisoformat(normalized)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3] + ' UTC'
    except (ValueError, TypeError):
        return ts


def show_logs(logs, device_id, update_id, total=None):
    """Display installation logs grouped by service, similar to a journal."""
    print(colorize("\n=== Online Update Installation Logs ===", Colors.BOLD + Colors.HEADER))
    print(colorize(f"    Device ID: {device_id}", Colors.DIM))
    print(colorize(f"    Update ID: {update_id}", Colors.DIM))
    if total is not None:
        print(colorize(f"    Total Entries: {len(logs)} of {total}", Colors.DIM))
    else:
        print(colorize(f"    Total Entries: {len(logs)}", Colors.DIM))
    print()

    if not logs:
        print("No log entries returned for this update.")
        return

    last_service = None
    for entry in logs:
        service = entry.get('service', '') or ''
        timestamp = format_timestamp(entry.get('timestamp'))
        message = entry.get('message', '')

        if service != last_service:
            label = service or "(unknown service)"
            print(colorize(f"\n[{label}]", Colors.CYAN + Colors.BOLD))
            last_service = service

        ts_formatted = colorize(timestamp, Colors.DIM)
        print(f"{ts_formatted}  {message}")
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def resolve_ids(args):
    """Return ``(device_id, update_id)`` from the CLI arguments."""
    if args.device_id and args.update_id:
        return args.device_id, args.update_id

    if args.url:
        parsed = parse_ids_from_url(args.url)
        if parsed is None:
            print(
                "Error: URL does not look like a Torizon Cloud device URL "
                "(expected: https://app.torizon.io/devices/<device>/updates/<update>)",
                file=sys.stderr,
            )
            sys.exit(2)
        return parsed

    print(
        "Error: must supply either a Torizon Cloud device URL or "
        "--device-id and --update-id",
        file=sys.stderr,
    )
    sys.exit(2)


def resolve_token(args):
    """Return the Bearer token from CLI/env/credentials-file."""
    token = args.token or os.environ.get('TORIZON_API_TOKEN')
    if token:
        return token

    if args.credentials:
        client_id, client_secret, token_endpoint = load_credentials(args.credentials)
        try:
            return fetch_token(client_id, client_secret, token_endpoint)
        except ApiError as e:
            if e.status in (401, 403):
                print(
                    "Error: token endpoint rejected the credentials (HTTP {}). "
                    "Check clientId/clientSecret in {}.".format(e.status, args.credentials),
                    file=sys.stderr,
                )
            else:
                print(f"Error: failed to fetch token from {token_endpoint}: {e}",
                      file=sys.stderr)
            sys.exit(1)

    print(
        "Error: no API token available. Pass --token, set TORIZON_API_TOKEN, "
        "or supply --credentials <file>.",
        file=sys.stderr,
    )
    sys.exit(2)


def main():
    parser = argparse.ArgumentParser(
        description='Display installation logs for an update from the Torizon Cloud API.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Parse a Torizon Cloud device URL
  %(prog)s 'https://app.torizon.io/devices/<device>/updates/<update>'

  # Use explicit IDs
  %(prog)s --device-id <device> --update-id <update>

  # Filter by systemd service
  %(prog)s <url> --service aktualizr.service

  # Dump raw JSON
  %(prog)s <url> --json

  # Provide credentials in a JSON file (fetching a token via OAuth2
  # client_credentials). The file should look like:
  #   {
  #     "clientId": "<yourClientId>",
  #     "clientSecret": "<yourClientSecret>",
  #     "tokenEndpoint": "https://kc.torizon.io/auth/realms/ota-users/protocol/openid-connect/token"
  #   }
  %(prog)s <url> --credentials torion-cred.json
""",
    )

    parser.add_argument(
        'url', nargs='?',
        help='Torizon Cloud device URL containing the device and update IDs',
    )
    parser.add_argument('--device-id', help='Device UUID (overrides URL)')
    parser.add_argument('--update-id', help='Update/Update-job UUID (overrides URL)')
    parser.add_argument(
        '--token', '-t',
        help='Bearer token for the Torizon API '
             '(defaults to the TORIZON_API_TOKEN environment variable)',
    )
    parser.add_argument(
        '--credentials', '-c', metavar='FILE',
        help='Path to a JSON file containing clientId/clientSecret/tokenEndpoint. '
             'A token is fetched automatically via OAuth2 client_credentials. '
             'Used when --token/TORIZON_API_TOKEN is not set.',
    )
    parser.add_argument(
        '--service', '-s',
        help='Only show log messages from this systemd unit (e.g. aktualizr.service)',
    )
    parser.add_argument(
        '--limit', type=int, default=DEFAULT_LIMIT,
        help=f'Page size when fetching logs (default: {DEFAULT_LIMIT})',
    )
    parser.add_argument(
        '--json', action='store_true',
        help='Print the raw JSON response (or fetched log messages) and exit',
    )

    args = parser.parse_args()
    device_id, update_id = resolve_ids(args)
    token = resolve_token(args)

    try:
        logs, total = fetch_all_logs(
            device_id,
            update_id,
            token,
            service=args.service,
            page_size=args.limit,
        )
    except ApiError as e:
        if e.status in (401, 403):
            print(
                "Error: API authentication failed (HTTP {}). "
                "Check that your Bearer token is valid and has access to this device.".format(e.status),
                file=sys.stderr,
            )
        elif e.status == 404:
            print(
                f"Error: no logs found for device {device_id} / update {update_id} "
                "(HTTP 404). The update may not have uploaded any installation logs.",
                file=sys.stderr,
            )
        elif e.status == 420:
            print("Error: rate limited by the Torizon API. Try again later.", file=sys.stderr)
        elif e.status == 429:
            print("Error: rate limited by the Torizon API. Try again later.", file=sys.stderr)
        else:
            print(f"Error: API request failed: {e}", file=sys.stderr)
        sys.exit(1)

    if args.json:
        out = {
            'deviceId': device_id,
            'updateId': update_id,
            'total': total,
            'count': len(logs),
            'values': logs,
        }
        print(json.dumps(out, indent=2))
        return

    show_logs(logs, device_id, update_id, total=total)


if __name__ == '__main__':
    main()
