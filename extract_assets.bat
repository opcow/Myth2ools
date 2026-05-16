@echo off
setlocal EnableExtensions EnableDelayedExpansion

if "%~2"=="" goto usage

set "TAGS_FOLDER=%~1"
set "MESH_TAG=%~2"

shift
shift

set "OUT_FOLDER=%MESH_TAG%"
if not "%~1"=="" (
    set "NEXT_ARG=%~1"
    if not "!NEXT_ARG:~0,2!"=="--" (
        set "OUT_FOLDER=%~1"
        shift
    )
)

set "SCRIPT_DIR=%~dp0"
set "CURRENT_DIR=%CD%\"
set "BIN_DIR="

if not exist "%TAGS_FOLDER%" (
    echo Input tag source not found: "%TAGS_FOLDER%"
    exit /b 1
)

call :try_bin_dir "%SCRIPT_DIR%build\Release\"
call :try_bin_dir "%SCRIPT_DIR%build\Debug\"
call :try_bin_dir "%SCRIPT_DIR%bin\"
call :try_bin_dir "%CURRENT_DIR%build\Release\"
call :try_bin_dir "%CURRENT_DIR%build\Debug\"
call :try_bin_dir "%CURRENT_DIR%bin\"
call :try_bin_dir "%SCRIPT_DIR%"
call :try_bin_dir "%CURRENT_DIR%"
call :try_bin_dir "%SCRIPT_DIR%..\build\Release\"
call :try_bin_dir "%SCRIPT_DIR%..\build\Debug\"

if "%BIN_DIR%"=="" goto missing_tools

set "EXTRACT_ARGS="
set "MODEL_ARGS="

:parse_args
if "%~1"=="" goto run_tools
if /I "%~1"=="--ora" (
    set "EXTRACT_ARGS=%EXTRACT_ARGS% --ora"
    shift
    goto parse_args
)
if /I "%~1"=="--overwrite" (
    set "MODEL_ARGS=%MODEL_ARGS% --overwrite"
    shift
    goto parse_args
)
if /I "%~1"=="--animation-frame" (
    if "%~2"=="" goto usage
    set "MODEL_ARGS=%MODEL_ARGS% --animation-frame %~2"
    shift
    shift
    goto parse_args
)
echo Unknown option: %~1
goto usage

:run_tools
echo Extracting map assets...
"%BIN_DIR%extract_map.exe" "%TAGS_FOLDER%" "%MESH_TAG%" --out "%OUT_FOLDER%" %EXTRACT_ARGS%
if errorlevel 1 exit /b 1
if not exist "%OUT_FOLDER%\manifest.json" (
    echo extract_map did not produce "%OUT_FOLDER%\manifest.json"
    exit /b 1
)

echo Exporting terrain mesh...
"%BIN_DIR%export_mesh.exe" "%OUT_FOLDER%"
if errorlevel 1 exit /b 1

echo Exporting water mesh...
"%BIN_DIR%export_water_mesh.exe" "%OUT_FOLDER%"
if errorlevel 1 exit /b 1

echo Exporting map objects...
"%BIN_DIR%export_map_objects.exe" "%TAGS_FOLDER%" "%OUT_FOLDER%" "%OUT_FOLDER%\assets\terrain\displacement.obj" %MODEL_ARGS%
if errorlevel 1 exit /b 1

echo Exporting map actions...
"%BIN_DIR%export_map_actions.exe" "%OUT_FOLDER%"
if errorlevel 1 exit /b 1

echo Done. Output written to "%OUT_FOLDER%".
exit /b 0

:usage
echo Usage:
echo   extract_assets.bat ^<tags_folder^|plugin_file^> ^<meshtag^> [output_folder] [--ora] [--overwrite] [--animation-frame first^|none^|all]
echo.
echo Runs extract_map, export_mesh, export_water_mesh, export_map_objects, and export_map_actions.
exit /b 1

:try_bin_dir
if not "%BIN_DIR%"=="" exit /b 0
if exist "%~1extract_map.exe" if exist "%~1export_mesh.exe" if exist "%~1export_water_mesh.exe" if exist "%~1export_map_objects.exe" if exist "%~1export_map_actions.exe" set "BIN_DIR=%~1"
exit /b 0

:missing_tools
echo Could not find the extraction executables.
echo Checked:
echo   "%SCRIPT_DIR%"
echo   "%CURRENT_DIR%"
echo   "%SCRIPT_DIR%build\Release\"
echo   "%SCRIPT_DIR%build\Debug\"
echo   "%SCRIPT_DIR%bin\"
echo   "%CURRENT_DIR%build\Release\"
echo   "%CURRENT_DIR%build\Debug\"
echo   "%CURRENT_DIR%bin\"
echo   "%SCRIPT_DIR%..\build\Release\"
echo   "%SCRIPT_DIR%..\build\Debug\"
echo.
echo Expected to find extract_map.exe, export_mesh.exe, export_water_mesh.exe, export_map_objects.exe, and export_map_actions.exe in the same folder.
exit /b 1
