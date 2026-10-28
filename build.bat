@echo off
rem Builds samplosis.clap with Visual Studio's compiler. "build.bat test" also builds tests\host.exe.
rem CLAP_SDK (checkout root of free-audio/clap) overrides the default include folder.
setlocal
set "CLAPINC=E:\samplepacks\_build\clap\include"
if not "%CLAP_SDK%"=="" set "CLAPINC=%CLAP_SDK%\include"
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set "OBJ=%TEMP%\samplosis_obj"
if not exist "%OBJ%" mkdir "%OBJ%"
rem Rubber Band, C++ (warnings about narrowing silenced); src is on the include path
cl /nologo /c /O2 /W3 /wd4244 /wd4267 /MT /EHsc /I src /Fo"%OBJ%\rb.obj" src\rb.cpp || exit /b 1
rem The try/catch shim around Rubber Band: /EHs, so calls into its extern "C" API may throw and are caught
cl /nologo /c /O2 /W3 /MT /EHs /I src /Fo"%OBJ%\rbsafe.obj" src\rbsafe.cpp || exit /b 1
cl /nologo /D_CRT_SECURE_NO_WARNINGS /O2 /W3 /LD /MT /I "%CLAPINC%" /Fo"%OBJ%\\" src\*.c "%OBJ%\rb.obj" "%OBJ%\rbsafe.obj" /Fe:samplosis.clap /link /NOLOGO user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib || exit /b 1
if /i "%1"=="test" (
  cl /nologo /D_CRT_SECURE_NO_WARNINGS /O2 /W3 /MT /I "%CLAPINC%" /I src /Fo"%OBJ%\\" tests\host.c /Fe:tests\host.exe /link /NOLOGO user32.lib ole32.lib || exit /b 1
)
del samplosis.exp samplosis.lib 2>nul
