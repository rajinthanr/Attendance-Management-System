@echo off
rem Starts the Attendance Logger app in its own window. Double-click this file.
rem Needs PySide6 (Qt), in .venv next to this file or in Python itself. One-time setup:
rem     python -m venv .venv  then  .venv\Scripts\pip install -r requirements.txt
rem (python attendance_app.py opens the same window.)
cd /d "%~dp0"
if exist ".venv\Scripts\python.exe" (
    ".venv\Scripts\python.exe" attendance_qt.py %*
    if errorlevel 1 pause
    goto :end
)
where python >nul 2>nul
if %errorlevel%==0 (
    python attendance_qt.py %*
    if errorlevel 1 pause
    goto :end
)
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 attendance_qt.py %*
    if errorlevel 1 pause
    goto :end
)
echo.
echo Python 3 was not found. Install it from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH"), then double-click this file again.
echo.
pause
:end
