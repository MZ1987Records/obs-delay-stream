@echo off
chcp 437 > nul
echo Releasing port 19000...
for /f "tokens=5" %%a in ('netstat -ano ^| findstr :19000 ^| findstr LISTENING') do (
    echo Killing PID %%a
    taskkill /PID %%a /F >nul 2>&1
)
echo Port 19000 released. You can now restart OBS.
pause
