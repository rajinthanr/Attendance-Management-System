#!/bin/sh
# Starts the Attendance Logger app (macOS and Linux). Run:  sh start.sh
cd "$(dirname "$0")" || exit 1
if command -v python3 >/dev/null 2>&1; then
    exec python3 attendance_app.py "$@"
fi
echo "Python 3 was not found. Install it from https://www.python.org/downloads/"
exit 1
