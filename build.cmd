@echo off
rem Needs a MinGW-w64 gcc on PATH (e.g. MSYS2 ucrt64). Output: agentbar.exe, no runtime deps.
gcc -std=c11 -Os -s -municode -mwindows -Wall -Wextra -fno-asynchronous-unwind-tables -fno-ident ^
    -o agentbar.exe agentbar.c -luser32 -lgdi32 -lshell32 -ladvapi32 -ldwmapi -lole32 -loleaut32 -luuid -lntdll
