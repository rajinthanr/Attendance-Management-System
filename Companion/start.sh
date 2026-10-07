#!/bin/sh
# Starts the Attendance Logger app (macOS and Linux) in its own window. Run:  sh start.sh
# Needs PySide6 (Qt), in .venv next to this file or in the system Python. One-time setup:
#     python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
# (python3 attendance_app.py opens the same window.)
cd "$(dirname "$0")" || exit 1
if [ -x .venv/bin/python ]; then
    exec .venv/bin/python attendance_qt.py "$@"
fi
if command -v python3 >/dev/null 2>&1; then
    exec python3 attendance_qt.py "$@"
fi
echo "Python 3 was not found. Install it from https://www.python.org/downloads/"
exit 1
