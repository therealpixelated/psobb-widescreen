@echo off
REM ReShade installer launcher for the PSOBB widescreen package.
REM Double-click me: I just run the PowerShell install scripts in this folder.
REM Pass-through switches still work, e.g.:  install.bat -GameDir "D:\Games\PSOBB"
setlocal
cd /d "%~dp0"

REM Prefer PowerShell 7 (pwsh) if present, else Windows PowerShell.
where pwsh >nul 2>&1 && (set "PS=pwsh") || (set "PS=powershell")

echo(
echo ==================================================
echo   PSOBB ReShade installer
echo ==================================================
echo   1. Detect my d3d8 wrapper          (read-only)
echo   2. Install ReShade - dgVoodoo2 base
echo   3. Install ReShade - crosire d3d8to9 base
echo   4. Exit
echo(
set "choice="
set /p choice="Choose [1-4]: "

if "%choice%"=="1" %PS% -ExecutionPolicy Bypass -File "%~dp0detect_wrapper.ps1" %*
if "%choice%"=="2" %PS% -ExecutionPolicy Bypass -File "%~dp0install_dgvoodoo.ps1" %*
if "%choice%"=="3" %PS% -ExecutionPolicy Bypass -File "%~dp0install_crosire.ps1" %*
if "%choice%"=="4" goto :eof

echo(
pause
endlocal
