@echo off
setlocal EnableExtensions
if not exist "%~dp0Play Wolverine.bat" (
  echo Play Wolverine.bat is missing. Keep this file beside the original launcher.
  pause
  exit /b 1
)
echo.
echo Wolverine AMD tuning test profiles - no performance gain is assumed.
echo 1. Baseline: four pipeline workers, 20 ms wait
echo 2. Shorter wait: four workers, 5 ms wait
echo 3. Worker comparison: two workers, 20 ms wait
echo 4. Worker comparison: six workers, 20 ms wait
echo 5. Experimental asynchronous shaders, one worker and compiler lock enabled
echo 6. Diagnostic baseline: shader statistics and GPU timestamps
choice /c 123456 /n /m "Choose a profile [1-6]: "
set "PROFILE=%ERRORLEVEL%"
set "KYTY_PIPELINE_THREADS=4"
set "KYTY_PIPELINE_WAIT_MS=20"
set "KYTY_ASYNC_PIPELINES=1"
set "KYTY_RECORD_THREAD=1"
set "KYTY_ASYNC_SHADERS=0"
set "KYTY_ASYNC_SHADER_THREADS=1"
set "KYTY_RECOMPILER_LOCK=1"
set "KYTY_SHADER_STATS=0"
set "KYTY_GPU_TIME=0"
if "%PROFILE%"=="2" set "KYTY_PIPELINE_WAIT_MS=5"
if "%PROFILE%"=="3" set "KYTY_PIPELINE_THREADS=2"
if "%PROFILE%"=="4" set "KYTY_PIPELINE_THREADS=6"
if "%PROFILE%"=="5" set "KYTY_ASYNC_SHADERS=1"
if "%PROFILE%"=="6" set "KYTY_SHADER_STATS=1"
if "%PROFILE%"=="6" set "KYTY_GPU_TIME=1"
echo.
echo Profile %PROFILE%: %KYTY_PIPELINE_THREADS% pipeline workers, %KYTY_PIPELINE_WAIT_MS% ms wait.
if "%PROFILE%"=="5" echo Experimental: some effects may appear late. Use profile 1 to compare.
call "%~dp0Play Wolverine.bat" %*
exit /b %ERRORLEVEL%
