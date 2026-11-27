@echo off
rem Publishes samplosis.clap as the GitHub release v<SMP_VERSION> (src\samplosis.h), from the commit checked out.
rem Raise SMP_VERSION and commit first: the Update Samplosis button installs a release only when its version is newer.
rem Needs gh, signed in. https://github.com/president-lion/samplosis/releases/latest/download/samplosis.clap is
rem always the newest release's file.
setlocal
cd /d "%~dp0"
set "VER="
for /f "tokens=3" %%v in ('findstr /b /c:"#define SMP_VERSION " src\samplosis.h') do set "VER=%%~v"
if "%VER%"=="" ( echo SMP_VERSION not found in src\samplosis.h & exit /b 1 )
gh release view v%VER% >nul 2>&1 && ( echo v%VER% is already released: raise SMP_VERSION in src\samplosis.h & exit /b 1 )
git diff --quiet HEAD || ( echo Commit your changes first & exit /b 1 )
call build.bat || exit /b 1
for /f %%h in ('git rev-parse HEAD') do set "HEAD=%%h"
git push origin HEAD || exit /b 1
gh release create v%VER% samplosis.clap --target %HEAD% --title "Samplosis %VER%" --generate-notes || exit /b 1
echo Released v%VER%
