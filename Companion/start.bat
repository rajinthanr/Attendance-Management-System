@echo off
rem Starts the Attendance Logger app in its own window. Double-click this file.
rem (The browser version is still there: python attendance_app.py)
cd /d "%~dp0"
where python >nul 2>nul
if %errorlevel%==0 (
    python attendance_gui.py %*
    if errorlevel 1 pause
    goto :end
)
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 attendance_gui.py %*
    if errorlevel 1 pause
    goto :end
)
echo.
echo Python 3 was not found. Install it from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH"), then double-click this file again.
echo.
pause
:end
