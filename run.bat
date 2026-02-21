@echo off
C:\Users\user\Desktop\projects\vtol-co-pilot\Scripts\cmake.exe --build C:\Users\user\Desktop\projects\vtol-co-pilot\build --config Release
if %ERRORLEVEL% EQU 0 (
    cd /d C:\Users\user\Desktop\projects\vtol-co-pilot\build\Release
    vtol-copilot.exe
) else (
    echo.
    echo BUILD FAILED
    pause
)
