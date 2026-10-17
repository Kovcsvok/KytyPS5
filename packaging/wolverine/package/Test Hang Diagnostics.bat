@echo off
setlocal EnableExtensions
set "HERE=%~sdp0"
set "KYTY_TEST_USERDATA=%HERE%userdata-hang"
if not exist "%KYTY_TEST_USERDATA%" mkdir "%KYTY_TEST_USERDATA%"
if not exist "%KYTY_TEST_USERDATA%\_SaveData" if exist "%HERE%userdata\_SaveData" (
  xcopy "%HERE%userdata\_SaveData" "%KYTY_TEST_USERDATA%\_SaveData\" /e /i /q >nul
  if errorlevel 1 exit /b 1
)
rem GPU progress markers (VK_AMD_buffer_marker): after a device loss the log names the draw or
rem dispatch the GPU hung in, with its shader hashes ("GPU hang suspect").
set KYTY_GPU_MARKERS=1
set KYTY_HOST_BREADCRUMBS=1
rem Every shader loop leaves after 4096 iterations and logs "WatchdogTrip" with the shader hash
rem instead of hanging the GPU. It changes shader translation, so this folder keeps its own cache.
set KYTY_SHADER_LOOP_WATCHDOG=1
echo Hang diagnostics: GPU markers and loop watchdog on (separate userdata-hang folder).
call "%HERE%Play Wolverine.bat" %*
