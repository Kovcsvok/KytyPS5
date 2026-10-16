@echo off
setlocal EnableExtensions
set "HERE=%~sdp0"
set "KYTY_TEST_USERDATA=%HERE%userdata-combat-lifetime"
if not exist "%KYTY_TEST_USERDATA%" mkdir "%KYTY_TEST_USERDATA%"
if not exist "%KYTY_TEST_USERDATA%\_SaveData" if exist "%HERE%userdata\_SaveData" (
  xcopy "%HERE%userdata\_SaveData" "%KYTY_TEST_USERDATA%\_SaveData\" /e /i /q >nul
  if errorlevel 1 exit /b 1
)
set KYTY_BINDLESS_EVICT=0
set KYTY_BINDLESS_SLOT_REUSE=0
set KYTY_HOST_BREADCRUMBS=1
set KYTY_VS_LANE_WRAP32=1
set KYTY_MESH_WAVE_PASSES=1
set KYTY_PS_SUBGROUP=1
echo Combat lifetime trial: eviction and slot reuse disabled. Higher VRAM use possible.
call "%HERE%Play Wolverine.bat" %*
