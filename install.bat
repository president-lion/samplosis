@echo off
rem Copies samplosis.clap to the CLAP folder.
copy /y "%~dp0samplosis.clap" "D:\clap\samplosis.clap" >nul
if errorlevel 1 (
  echo Copy failed. REAPER may have samplosis.clap loaded; close REAPER and try again.
  exit /b 1
)
echo Installed to D:\clap\samplosis.clap
