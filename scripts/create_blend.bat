@echo off
setlocal EnableExtensions EnableDelayedExpansion

if "%~1"=="" goto usage

set "MAP_FOLDER=%~1"
set "OUTPUT_BLEND=%~2"
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_PARENT=%SCRIPT_DIR%..\\"
set "CURRENT_DIR=%CD%\\"

call :find_blender
if "%BLENDER_EXE%"=="" goto missing_blender

set "PY_SCRIPT=%SCRIPT_DIR%..\\tools\\create_blend.py"
if not exist "%PY_SCRIPT%" set "PY_SCRIPT=%SCRIPT_PARENT%tools\\create_blend.py"
if not exist "%PY_SCRIPT%" set "PY_SCRIPT=%CURRENT_DIR%tools\\create_blend.py"
if not exist "%PY_SCRIPT%" goto missing_script

if "%OUTPUT_BLEND%"=="" (
    "%BLENDER_EXE%" --factory-startup --background --python "%PY_SCRIPT%" -- "%MAP_FOLDER%"
) else (
    "%BLENDER_EXE%" --factory-startup --background --python "%PY_SCRIPT%" -- "%MAP_FOLDER%" "%OUTPUT_BLEND%"
)
exit /b %ERRORLEVEL%

:find_blender
if not "%BLENDER_PATH%"=="" (
    set "BLENDER_EXE=%BLENDER_PATH%"
    exit /b 0
)
if exist "%SCRIPT_DIR%..\\blender_path.txt" (
    set /p BLENDER_EXE=<"%SCRIPT_DIR%..\\blender_path.txt"
    exit /b 0
)
if exist "%SCRIPT_PARENT%blender_path.txt" (
    set /p BLENDER_EXE=<"%SCRIPT_PARENT%blender_path.txt"
    exit /b 0
)
if exist "%CURRENT_DIR%blender_path.txt" (
    set /p BLENDER_EXE=<"%CURRENT_DIR%blender_path.txt"
    exit /b 0
)
where blender.exe >nul 2>nul
if not errorlevel 1 (
    set "BLENDER_EXE=blender.exe"
    exit /b 0
)
where blender >nul 2>nul
if not errorlevel 1 set "BLENDER_EXE=blender"
exit /b 0

:missing_blender
echo Could not find Blender.
echo Set BLENDER_PATH or create blender_path.txt beside the app folder.
echo Example blender_path.txt:
echo   C:\Program Files\Blender Foundation\Blender 4.3\blender.exe
exit /b 1

:missing_script
echo Could not find tools\\create_blend.py relative to this script.
exit /b 1

:usage
echo Usage:
echo   create_blend.bat ^<map_folder^> [output.blend]
echo.
echo Set BLENDER_PATH or put Blender's executable path in blender_path.txt.
exit /b 1
