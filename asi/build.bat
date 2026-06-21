@echo off
REM Build pso_widescreen.asi — standalone widescreen / resolution-override ASI.
REM See pso_widescreen.c header for the hook strategy.
REM
REM Output drops next to this script as pso_widescreen.asi; deploy by
REM copying it to PSOBB.IO\patches\.

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"

del pso_widescreen.obj anzz1_widescreen.obj pso_widescreen.exp pso_widescreen.lib pso_widescreen.asi p_*.obj 2>nul

REM ----- MinHook (for Phase-3 sprite-primitive inline hook 2026-05-10) -----
set MH=..\_shared\minhook
cl /nologo /c /O2 /W3 /GS- /Fop_buf.obj  /I %MH%\include  %MH%\src\buffer.c
cl /nologo /c /O2 /W3 /GS- /Fop_hook.obj /I %MH%\include  %MH%\src\hook.c
cl /nologo /c /O2 /W3 /GS- /Fop_trm.obj  /I %MH%\include  %MH%\src\trampoline.c
cl /nologo /c /O2 /W3 /GS- /Fop_hde.obj  /I %MH%\include  %MH%\src\hde\hde32.c

cl /nologo /LD /O2 /W3 /GS- /I %MH%\include ^
   pso_widescreen.c asset_registry.c asset_registry_generated.c mod_boot_poster.c mod_video.c ^
   p_buf.obj p_hook.obj p_trm.obj p_hde.obj ^
   kernel32.lib user32.lib gdi32.lib winmm.lib ^
   mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib ^
   /link /SUBSYSTEM:WINDOWS /DLL /MACHINE:X86 ^
         /OUT:pso_widescreen.asi
if errorlevel 1 exit /b 1

echo ----- pso_widescreen.asi built -----
dir /b pso_widescreen.asi
