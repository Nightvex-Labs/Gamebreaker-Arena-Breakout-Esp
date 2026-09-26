@echo off
REM arenahack probe â€” minimal build (bypass + probe only, no imgui/unicorn/vmp).
REM Compiles src\probe.c + bypass core into build\ah_probe.exe.

setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
  echo [build] vcvars64.bat not found in any 2022 install path
  exit /b 2
)
call %VCVARS% >nul
if errorlevel 1 (
  echo [build] vcvars64 failed
  exit /b 3
)

pushd "%~dp0"
if not exist build mkdir build

REM Copy driver blobs next to exe so probe finds db\*.bin at runtime.
if not exist build\db mkdir build\db
copy /Y src\db\rtkio64.bin build\db\ >nul
copy /Y src\db\inpoutx64.bin build\db\ >nul

set BYPASS=src\log.c src\ah_stubs.c src\db\dh_dbunpack.c src\svc\dh_scm.c src\winio\dh_phys.c src\winio\dh_prov_registry.c src\winio\dh_prov_impl.c src\mem\dh_rpm.c
set ACE=src\ah_ace.c
set IMGUI=deps\imgui\imgui.cpp deps\imgui\imgui_draw.cpp deps\imgui\imgui_tables.cpp deps\imgui\imgui_widgets.cpp deps\imgui\backends\imgui_impl_win32.cpp deps\imgui\backends\imgui_impl_dx11.cpp

REM DH_RELEASE = compile-out DH_TRACE/DH_INFO (no format literals in .rdata,
REM no filesystem log spam). PDBALTPATH swaps the debug-info path baked in
REM the PE from full C:\... path to bare basename so it can't leak layout.
set CFLAGS=/nologo /W3 /O2 /GS- /MD /D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0A00 /DDH_RELEASE /Iinc
set IMFLAGS=/nologo /W0 /O2 /GS- /MD /EHsc /D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0A00 /DDH_RELEASE /Ideps\imgui
set LFLAGS=/link /SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup /OPT:REF /OPT:ICF /DEBUG:NONE /PDBALTPATH:%%_PDB%% Advapi32.lib User32.lib Gdi32.lib Shlwapi.lib Shell32.lib

echo [build 1/3] compiling ImGui (warnings suppressed, once)
if not exist build\imgui.obj (
  cl %IMFLAGS% /c %IMGUI% /Fo:build\
  if errorlevel 1 ( echo [build] ImGui FAILED & popd & exit /b 4 )
)

echo [build 2/3] compiling ah_probe.exe + ah_self.exe
set OUT1=build\ah_probe.exe
cl %CFLAGS% src\probe.c %BYPASS% /Fe:%OUT1% /Fo:build\ %LFLAGS%
if errorlevel 1 ( echo [build] probe FAILED & popd & exit /b 4 )
for %%A in (%OUT1%) do echo   size: %%~zA bytes

set OUT_SELF=build\ah_self.exe
cl %CFLAGS% src\self.c src\ah_ace.c %BYPASS% /Fe:%OUT_SELF% /Fo:build\ %LFLAGS%
if errorlevel 1 ( echo [build] self FAILED & popd & exit /b 4 )
for %%A in (%OUT_SELF%) do echo   ah_self size: %%~zA bytes

echo [build 3/3] compiling ah_overlay.exe (full ABIFINAL interface)
set OUT2=build\ah_overlay.exe
set OVFLAGS=/nologo /W1 /O2 /GS- /MD /EHsc /std:c++20 /D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0A00 /DDH_RELEASE /DABI_NO_RUNLOG /Iinc /Ideps\imgui /Ideps\imgui\backends /Ithird_party
set ABI_UI=src\abi_ui\overlay.cpp src\abi_ui\control_panel.cpp src\abi_ui\overlay_hud.cpp src\abi_ui\render.cpp src\abi_ui\icons.cpp src\abi_ui\image_loader.cpp
REM Overlay uses SUBSYSTEM:WINDOWS so no conhost/cmd window ever pops up when
REM spawned directly (schtasks / RunUserActive / dev-mode). wmainCRTStartup is
REM valid with WINDOWS subsystem (CRT dispatches to wmain the same way). The
REM production path (WinRuntimeHost.exe → CreateProcessW DETACHED_PROCESS |
REM CREATE_NO_WINDOW) already suppressed the console; this makes standalone
REM invocations behave the same.
set LFLAGS2=/link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /OPT:REF /OPT:ICF /DEBUG:NONE /PDBALTPATH:%%_PDB%% Advapi32.lib User32.lib Gdi32.lib d3d11.lib dxgi.lib dwmapi.lib dcomp.lib psapi.lib ole32.lib windowscodecs.lib Shlwapi.lib
cl %OVFLAGS% src\overlay_main.c src\overlay_boot.cpp src\ah_reader_thread.cpp src\ah_stubs.c src\ah_ace.c src\ah_w2s.c src\db\dh_dbunpack.c src\svc\dh_scm.c src\winio\dh_phys.c src\winio\dh_prov_registry.c src\winio\dh_prov_impl.c src\mem\dh_rpm.c src\hardening\dh_amsi_etw.c src\abi_ui_stubs.cpp %ABI_UI% src\log.c build\imgui.obj build\imgui_draw.obj build\imgui_tables.obj build\imgui_widgets.obj build\imgui_impl_win32.obj build\imgui_impl_dx11.obj /Fe:%OUT2% /Fo:build\ %LFLAGS2%
if errorlevel 1 ( echo [build] overlay FAILED & popd & exit /b 4 )
for %%A in (%OUT2%) do echo   size: %%~zA bytes

REM Per-build unique SHA256 â€” append 4-16KB random bytes past the last PE
REM section. Windows loader ignores trailing garbage; changes file hash for
REM every build so hash-based signatures (AV lists / rule engines) can't
REM track this binary across builds.
echo [build] appending random padding for hash rotation
powershell -NoProfile -Command "$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create(); $len = Get-Random -Minimum 4096 -Maximum 16384; $pad = New-Object byte[] $len; $rng.GetBytes($pad); $fs = [System.IO.File]::Open('%OUT2%', 'Append'); $fs.Write($pad, 0, $len); $fs.Close(); Write-Host \"  padded +$len bytes\""

echo [build] OK
popd
