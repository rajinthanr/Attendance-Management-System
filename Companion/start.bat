@echo off
rem Starts the Attendance Logger app. Double-click this file.
cd /d "%~dp0"
where python >nul 2>nul
if %errorlevel%==0 (
    python attendance_app.py %*
    goto :end
)
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 attendance_app.py %*
    goto :end
)
echo.
echo Python 3 was not found. Install it from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH"), then double-click this file again.
echo.
pause
:end
