#!/usr/bin/env python3
"""
Offline Update Logs Viewer

Command-line tool to display contents of update-logs.db files created
during offline updates.

Usage:
    # List all installs
    ./offline-logs-viewer.py /media/usb/update-logs.db

    # Export manifest for upload
    ./offline-logs-viewer.py /media/usb/update-logs.db --install 1 --manifest

    # Show systemd journal logs for an install
    ./offline-logs-viewer.py /media/usb/update-logs.db --install 1 --logs
"""

import argparse
import json
import os
import sqlite3
import sys
from datetime import datetime, timezone


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


def format_timestamp(timestamp_us):
    """Format a timestamp (in microseconds since epoch) as ISO 8601."""
    if timestamp_us is None:
        return "N/A"
    try:
        dt = datetime.fromtimestamp(timestamp_us / 1_000_000, tz=timezone.utc)
        return dt.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3] + ' UTC'
    except (ValueError, OSError):
        return f"Invalid timestamp: {timestamp_us}"


def open_database(db_path):
    """Open the SQLite database and return a connection."""
    if not os.path.exists(db_path):
        print(f"Error: Database file not found: {db_path}", file=sys.stderr)
        sys.exit(1)

    try:
        conn = sqlite3.connect(db_path)
        conn.row_factory = sqlite3.Row
        return conn
    except sqlite3.Error as e:
        print(f"Error opening database: {e}", file=sys.stderr)
        sys.exit(1)


def get_installs(conn):
    """Get all install records from the database."""
    cursor = conn.cursor()
    cursor.execute("""
        SELECT id, device_id, name, version, report_counter, manifest
        FROM installs
        ORDER BY id
    """)
    return cursor.fetchall()


def get_install_by_id(conn, install_id):
    """Get a specific install record by ID."""
    cursor = conn.cursor()
    cursor.execute("""
        SELECT id, device_id, name, version, report_counter, manifest
        FROM installs
        WHERE id = ?
    """, (install_id,))
    return cursor.fetchone()


def get_log_count(conn, install_id):
    """Get the count of log entries for an install."""
    cursor = conn.cursor()
    cursor.execute("SELECT COUNT(*) FROM logs WHERE install_id = ?", (install_id,))
    return cursor.fetchone()[0]


def get_report_count(conn, install_id):
    """Get the count of report entries for an install."""
    cursor = conn.cursor()
    cursor.execute("SELECT COUNT(*) FROM reports WHERE install_id = ?", (install_id,))
    return cursor.fetchone()[0]


def get_logs(conn, install_id):
    """Get all log entries for an install."""
    cursor = conn.cursor()
    cursor.execute("""
        SELECT id, timestamp, service, message
        FROM logs
        WHERE install_id = ?
        ORDER BY id
    """, (install_id,))
    return cursor.fetchall()


def show_logs(conn, install_id):
    """Display the systemd journal logs for a specific install."""
    install = get_install_by_id(conn, install_id)

    if install is None:
        print(f"Error: Install #{install_id} not found", file=sys.stderr)
        sys.exit(1)

    logs = get_logs(conn, install_id)

    if not logs:
        print(f"No log entries found for Install #{install_id}")
        return

    name = install['name'] or "(unknown)"
    print(colorize(f"\n=== Systemd Journal Logs for Install #{install_id} ===", Colors.BOLD + Colors.HEADER))
    print(colorize(f"    Update: {name}\n", Colors.DIM))

    # Track service changes for visual grouping
    last_service = None

    for log in logs:
        timestamp = format_timestamp(log['timestamp'])
        service = log['service']
        message = log['message']

        # Show service name when it changes
        if service != last_service:
            print(colorize(f"\n[{service}]", Colors.CYAN + Colors.BOLD))
            last_service = service

        # Format: timestamp message
        ts_formatted = colorize(timestamp, Colors.DIM)
        print(f"{ts_formatted}  {message}")

    print()  # Trailing newline


def list_installs(conn):
    """List all installs with their status."""
    installs = get_installs(conn)

    if not installs:
        print("No installs found in database.")
        return

    print(colorize("\n=== Offline Update Installs ===\n", Colors.BOLD + Colors.HEADER))

    for install in installs:
        install_id = install['id']
        device_id = install['device_id']
        name = install['name'] or "(unknown)"
        version = install['version']
        report_counter = install['report_counter']
        manifest = install['manifest']

        # Determine status
        if manifest is not None and report_counter is not None:
            status = colorize("COMPLETE", Colors.GREEN)
        else:
            status = colorize("IN-PROGRESS", Colors.YELLOW)

        log_count = get_log_count(conn, install_id)
        report_count = get_report_count(conn, install_id)

        print(colorize(f"Install #{install_id}", Colors.BOLD + Colors.CYAN))
        print(f"  Status:         {status}")
        print(f"  Device ID:      {colorize(device_id, Colors.DIM)}")
        print(f"  Update Name:    {name}")
        print(f"  Version:        {version}")
        if report_counter is not None:
            print(f"  Report Counter: {report_counter}")
        print(f"  Log Entries:    {log_count}")
        print(f"  Report Events:  {report_count}")
        print(f"  Has Manifest:   {'Yes' if manifest else 'No'}")
        print()


def export_manifest(conn, install_id):
    """Export the manifest for a specific install as JSON."""
    install = get_install_by_id(conn, install_id)

    if install is None:
        print(f"Error: Install #{install_id} not found", file=sys.stderr)
        sys.exit(1)

    manifest = install['manifest']

    if manifest is None:
        print(f"Error: Install #{install_id} has no manifest (install may be in progress)",
              file=sys.stderr)
        sys.exit(1)

    # Pretty-print the JSON if possible
    try:
        manifest_json = json.loads(manifest)
        print(json.dumps(manifest_json, indent=2))
    except json.JSONDecodeError:
        # If it's not valid JSON, print it as-is
        print(manifest)


def show_install_summary(conn, install_id):
    """Show a summary of a specific install."""
    install = get_install_by_id(conn, install_id)

    if install is None:
        print(f"Error: Install #{install_id} not found", file=sys.stderr)
        sys.exit(1)

    device_id = install['device_id']
    name = install['name'] or "(unknown)"
    version = install['version']
    report_counter = install['report_counter']
    manifest = install['manifest']

    # Determine status
    if manifest is not None and report_counter is not None:
        status = colorize("COMPLETE", Colors.GREEN)
    else:
        status = colorize("IN-PROGRESS", Colors.YELLOW)

    log_count = get_log_count(conn, install_id)
    report_count = get_report_count(conn, install_id)

    print(colorize(f"\n=== Install #{install_id} Summary ===\n", Colors.BOLD + Colors.HEADER))
    print(f"  Status:         {status}")
    print(f"  Device ID:      {colorize(device_id, Colors.DIM)}")
    print(f"  Update Name:    {name}")
    print(f"  Version:        {version}")
    if report_counter is not None:
        print(f"  Report Counter: {report_counter}")
    print(f"  Log Entries:    {log_count}")
    print(f"  Report Events:  {report_count}")
    print(f"  Has Manifest:   {'Yes' if manifest else 'No'}")

    # If manifest exists, try to extract installation result
    if manifest:
        try:
            manifest_json = json.loads(manifest)
            signed = manifest_json.get('signed', {})
            install_report = signed.get('installation_report', {})
            report = install_report.get('report', {})
            result = report.get('result', {})

            if result:
                success = result.get('success', False)
                code = result.get('code', 'UNKNOWN')
                desc = result.get('description', '')

                result_str = colorize(code, Colors.GREEN if success else Colors.RED)
                print(f"\n  Installation Result:")
                print(f"    Code:        {result_str}")
                print(f"    Success:     {success}")
                if desc:
                    print(f"    Description: {desc}")

                # Show correlation ID if available
                correlation_id = report.get('correlation_id')
                if correlation_id:
                    print(f"    Update ID:   {correlation_id}")

                # Show per-ECU results
                items = report.get('items', [])
                if items:
                    print(f"\n  ECU Results:")
                    for item in items:
                        ecu = item.get('ecu', 'unknown')[:16] + '...'
                        item_result = item.get('result', {})
                        item_code = item_result.get('code', 'UNKNOWN')
                        item_success = item_result.get('success', False)
                        item_status = colorize(item_code, Colors.GREEN if item_success else Colors.RED)
                        print(f"    - {ecu}: {item_status}")
        except (json.JSONDecodeError, KeyError, TypeError):
            pass  # Ignore errors parsing manifest

    print()


def main():
    parser = argparse.ArgumentParser(
        description='View contents of offline update logs database',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # List all installs
  %(prog)s /media/usb/update-logs.db

  # Show summary for specific install
  %(prog)s /media/usb/update-logs.db --install 1

  # Export manifest for upload
  %(prog)s /media/usb/update-logs.db --install 1 --manifest > manifest.json

  # Show systemd journal logs for an install
  %(prog)s /media/usb/update-logs.db --install 1 --logs
"""
    )

    parser.add_argument('database', help='Path to update-logs.db file')
    parser.add_argument('--install', '-i', type=int, metavar='ID',
                        help='Show details for specific install ID')
    parser.add_argument('--manifest', '-m', action='store_true',
                        help='Export manifest as JSON (requires --install)')
    parser.add_argument('--logs', '-l', action='store_true',
                        help='Show systemd journal logs (requires --install)')

    args = parser.parse_args()

    # Validate arguments
    if args.manifest and args.install is None:
        parser.error("--manifest requires --install to specify which install")
    if args.logs and args.install is None:
        parser.error("--logs requires --install to specify which install")

    conn = open_database(args.database)

    try:
        if args.manifest:
            export_manifest(conn, args.install)
        elif args.logs:
            show_logs(conn, args.install)
        elif args.install is not None:
            show_install_summary(conn, args.install)
        else:
            list_installs(conn)
    finally:
        conn.close()


if __name__ == '__main__':
    main()
