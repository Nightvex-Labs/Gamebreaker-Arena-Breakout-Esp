@echo off
REM arenahack launcher — outputs build\App.exe. Reads ah_bundle.kfpl from
REM same directory as itself at runtime.

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
if errorlevel 1 (echo [build] vcvars64 failed & exit /b 3)

pushd "%~dp0"
if not exist build mkdir build

echo [build] compiling launcher -^> build\WinRuntimeHost.exe
set CFLAGS=/nologo /W3 /O2 /GS- /MT /D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0A00
set LFLAGS=/link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /OPT:REF /OPT:ICF /LIBPATH:deps\vmprotect\lib Bcrypt.lib Crypt32.lib Advapi32.lib Kernel32.lib User32.lib Shlwapi.lib VMProtectSDK64.lib
cl %CFLAGS% src\ah_launcher.c /Fe:build\WinRuntimeHost.exe /Fo:build\ %LFLAGS%
if errorlevel 1 (echo [build] FAILED & popd & exit /b 4)

REM Stage freetype.dll next to the launcher exe. The sidecar-DLL logic in
REM ah_launcher.c (kSidecarDlls list) copies whatever DLLs it finds next to
REM itself into %TEMP%\<hex>\ so the spawned overlay child can resolve its
REM freetype import. Without freetype.dll here the launcher logs
REM "sidecar copy fail freetype.dll gle=2" and the overlay falls back to
REM stb_truetype defaults.
if exist ..\deps\freetype\lib\freetype.dll copy /Y ..\deps\freetype\lib\freetype.dll build\ >nul

echo [build] OK -^> build\WinRuntimeHost.exe
dir /b build\WinRuntimeHost.exe
popd
