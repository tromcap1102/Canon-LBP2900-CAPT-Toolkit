@echo off
REM Design by Bruce Nguyen from CCTVWIKI.COM va Claude Code Max
REM Chay script PowerShell cai may in LBP2900 qua mang. Se tu xin quyen Admin (UAC).
REM Cach dung: double-click file nay, hoac chay tu cmd:
REM   cai-may-in-lbp2900-windows.bat 192.168.1.152
set SERVER_IP=%1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cai-may-in-lbp2900-windows.ps1" -ServerIP "%SERVER_IP%"
