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
rem The stretchers and the try/catch shim around them (Signalsmith Stretch is header only): /EHs, so calls into Rubber Band's extern "C" API may throw and are caught
cl /nologo /c /O2 /W3 /wd4244 /wd4267 /wd4305 /MT /EHs /I src /Fo"%OBJ%\stretch.obj" src\stretch.cpp || exit /b 1
rem The vendored decoders in src\codecs (BSD licensed; each folder's VERSION.txt names the release), with their
rem warnings off (/w): libogg, libopus (float, plain C), opusfile (no HTTP), WavPack, and Monkey's Audio (C++)
set "CODECS=src\codecs"
set "CO=%OBJ%\codecs"
if not exist "%CO%" mkdir "%CO%"
del /q "%CO%\*.obj" 2>nul
cl /nologo /c /O2 /w /MT /MP /I "%CODECS%\ogg\include" /Fo"%CO%\\" "%CODECS%\ogg\src\*.c" || exit /b 1
cl /nologo /c /O2 /w /MT /MP /DOPUS_BUILD /DUSE_ALLOCA /I "%CODECS%\opus\include" /I "%CODECS%\opus\celt" /I "%CODECS%\opus\silk" /Fo"%CO%\\" "%CODECS%\opus\src\*.c" "%CODECS%\opus\celt\*.c" "%CODECS%\opus\silk\*.c" || exit /b 1
cl /nologo /c /O2 /w /MT /MP /D_CRT_SECURE_NO_WARNINGS /I "%CODECS%\opusfile\include" /I "%CODECS%\ogg\include" /I "%CODECS%\opus\include" /Fo"%CO%\\" "%CODECS%\opusfile\src\*.c" || exit /b 1
cl /nologo /c /O2 /w /MT /MP /DWIN32 /D_CRT_SECURE_NO_DEPRECATE /DENABLE_DSD /DENABLE_LEGACY /I "%CODECS%\wavpack\include" /Fo"%CO%\\" "%CODECS%\wavpack\src\*.c" || exit /b 1
cl /nologo /c /O2 /w /MT /MP /EHsc /std:c++17 /DPLATFORM_WINDOWS /DWIN32 /I "%CODECS%\mac\Source\Shared" /I "%CODECS%\mac\Source\MACLib" /Fo"%CO%\\" "%CODECS%\mac\Source\MACLib\*.cpp" "%CODECS%\mac\Source\MACLib\Old\*.cpp" "%CODECS%\mac\Source\Shared\*.cpp" || exit /b 1
rem Our shim around Monkey's Audio: /EHsc, and every exception is caught inside it
cl /nologo /c /O2 /W3 /MT /EHsc /std:c++17 /DPLATFORM_WINDOWS /DWIN32 /I "%CODECS%\mac\Source\Shared" /I "%CODECS%\mac\Source\MACLib" /Fo"%OBJ%\mac_shim.obj" "%CODECS%\mac_shim.cpp" || exit /b 1
rem The plugin. Media Foundation (mfdecode.c) is delay-loaded, so the plugin loads on Windows without it (an N
rem edition); mfdecode.c looks for its DLLs before the first call
cl /nologo /D_CRT_SECURE_NO_WARNINGS /O2 /W3 /LD /MT /MP /I "%CLAPINC%" /I "%CODECS%\opusfile\include" /I "%CODECS%\ogg\include" /I "%CODECS%\opus\include" /I "%CODECS%\wavpack\include" /Fo"%OBJ%\\" src\*.c "%OBJ%\rb.obj" "%OBJ%\stretch.obj" "%OBJ%\mac_shim.obj" "%CO%\*.obj" /Fe:samplosis.clap /link /NOLOGO user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib oleacc.lib mfplat.lib mfreadwrite.lib mfuuid.lib delayimp.lib /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll || exit /b 1
if /i "%1"=="test" (
  cl /nologo /D_CRT_SECURE_NO_WARNINGS /O2 /W3 /MT /I "%CLAPINC%" /I src /Fo"%OBJ%\\" tests\host.c /Fe:tests\host.exe /link /NOLOGO user32.lib ole32.lib oleaut32.lib oleacc.lib || exit /b 1
)
del samplosis.exp samplosis.lib 2>nul
