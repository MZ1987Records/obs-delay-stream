@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 437 > nul
title obs-delay-stream Build Script
for %%I in ("%~dp0.") do set "PLUGIN_DIR=%%~fI"
set "PLUGIN_VERSION=unknown"
if exist "%PLUGIN_DIR%\CMakeLists.txt" (
    for /f "usebackq tokens=3 delims= )" %%V in (`findstr /B /C:"project(obs-delay-stream VERSION " "%PLUGIN_DIR%\CMakeLists.txt"`) do (
        set "PLUGIN_VERSION=%%V"
    )
)
set "BANNER_VERSION="
if not "%PLUGIN_VERSION%"=="unknown" set "BANNER_VERSION= v%PLUGIN_VERSION%"

echo ================================================================
echo  obs-delay-stream%BANNER_VERSION%  Build Script
echo ================================================================
echo.

rem Environment variables:
rem - OBS_SOURCE_DIR: OBS Studio source path (example: D:\dev\obs-studio).
rem   If not set, use third_party\obs-studio.
rem - OBS_LEGACY_INSTALL: 1=legacy Program Files layout, 0=recommended ProgramData layout (default: 0).
rem - OBS_CI: 1=CI mode (skip install and pause).
rem - You can define KEY=VALUE entries in build.env. See build.env.sample.
rem Options:
rem - --receiver-only / --receiver / /receiver-only / /receiver : build and install receiver files only.

set "OBS_SOURCE_DIR="
set "OBS_LEGACY_INSTALL="
set "OBS_CI="
set "RECEIVER_ONLY=0"
set "ENV_FILE=%PLUGIN_DIR%\build.env"
if exist "%ENV_FILE%" (
    for /f "usebackq eol=# tokens=1,* delims==" %%A in ("%ENV_FILE%") do (
        if /I "%%A"=="OBS_SOURCE_DIR" if not "%%B"=="" (
            set "OBS_SOURCE_DIR=%%B"
        )
        if /I "%%A"=="OBS_LEGACY_INSTALL" if not "%%B"=="" (
            set "OBS_LEGACY_INSTALL=%%B"
        )
        if /I "%%A"=="OBS_CI" if not "%%B"=="" (
            set "OBS_CI=%%B"
        )
    )
)
:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="--receiver-only" (
    set "RECEIVER_ONLY=1"
    shift
    goto :parse_args
)
if /I "%~1"=="--receiver" (
    set "RECEIVER_ONLY=1"
    shift
    goto :parse_args
)
if /I "%~1"=="/receiver-only" (
    set "RECEIVER_ONLY=1"
    shift
    goto :parse_args
)
if /I "%~1"=="/receiver" (
    set "RECEIVER_ONLY=1"
    shift
    goto :parse_args
)
if "%OBS_SOURCE_DIR%"=="" (
    set "OBS_SOURCE_DIR=%~1"
)
shift
goto :parse_args
:args_done
if "%OBS_SOURCE_DIR%"=="" set "OBS_SOURCE_DIR=%PLUGIN_DIR%\third_party\obs-studio"
set OBS_INSTALL_DIR=C:\ProgramData\obs-studio
if "%OBS_LEGACY_INSTALL%"=="" set OBS_LEGACY_INSTALL=0
if "%OBS_LEGACY_INSTALL%"=="1" set OBS_INSTALL_DIR=C:\Program Files\obs-studio
set BUILD_DIR=%PLUGIN_DIR%\build
set RECEIVER_BUILD_DIR=%BUILD_DIR%\receiver_dist
set CI_MODE=0
if /I "%OBS_CI%"=="1" set CI_MODE=1
if /I "%CI%"=="true" set CI_MODE=1
if /I "%CI%"=="1" set CI_MODE=1

if "%RECEIVER_ONLY%"=="1" goto :receiver_only

echo [Step 0] Checking environment...

where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] cmake not found. Install from: https://cmake.org/download/
    goto :error
)
echo   cmake: OK

where git >nul 2>&1
if errorlevel 1 (
    echo [ERROR] git not found. Install from: https://git-scm.com/
    goto :error
)
echo   git: OK

where npm >nul 2>&1
if errorlevel 1 (
    echo [ERROR] npm not found. Install Node.js 20+ from: https://nodejs.org/
    goto :error
)
echo   npm: OK

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo [ERROR] Visual Studio not found.
    goto :error
)
echo   Visual Studio: OK
echo.

echo ================================================================
echo  [Step 0b] Checking OBS source...
echo ================================================================
echo.

if exist "%OBS_SOURCE_DIR%\build_x64\libobs\RelWithDebInfo\obs.lib" goto :obs_found
if exist "%OBS_SOURCE_DIR%\build_x64\libobs\Debug\obs.lib" goto :obs_found
echo   OBS library not found. Building OBS...
goto :build_obs
:obs_found
echo   OBS library: found at %OBS_SOURCE_DIR%
goto :step1_libs

:build_obs
if not exist "%PLUGIN_DIR%\third_party" mkdir "%PLUGIN_DIR%\third_party"
if not exist "%OBS_SOURCE_DIR%\CMakeLists.txt" (
    echo   Cloning OBS Studio...
    git clone --recursive https://github.com/obsproject/obs-studio.git "%OBS_SOURCE_DIR%"
    if errorlevel 1 goto :error
)
cd /d "%OBS_SOURCE_DIR%"
cmake --list-presets
cmake --preset windows-x64
if errorlevel 1 goto :error
cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
if errorlevel 1 goto :error
echo   OBS build complete.
cd /d "%PLUGIN_DIR%"

:step1_libs
echo.
echo ================================================================
echo  [Step 1] Checking third-party libraries...
echo ================================================================
cd /d "%PLUGIN_DIR%"

if not exist "%PLUGIN_DIR%\third_party" mkdir "%PLUGIN_DIR%\third_party"

if exist "%PLUGIN_DIR%\third_party\websocketpp\websocketpp\server.hpp" goto :websocketpp_ok
if exist "%PLUGIN_DIR%\third_party\websocketpp" rmdir /s /q "%PLUGIN_DIR%\third_party\websocketpp"
echo   Cloning websocketpp...
git clone https://github.com/zaphoyd/websocketpp.git "%PLUGIN_DIR%\third_party\websocketpp"
if errorlevel 1 goto :error
echo   websocketpp: OK
goto :asio_check

:websocketpp_ok
echo   websocketpp: already exists

:asio_check
if exist "%PLUGIN_DIR%\third_party\asio\asio\include\asio.hpp" goto :asio_ok
if exist "%PLUGIN_DIR%\third_party\asio\include\asio.hpp" goto :asio_ok
if exist "%PLUGIN_DIR%\third_party\asio" rmdir /s /q "%PLUGIN_DIR%\third_party\asio"
echo   Cloning asio v1.18.2...
git clone --branch asio-1-18-2 --depth 1 https://github.com/chriskohlhoff/asio.git "%PLUGIN_DIR%\third_party\asio"
if errorlevel 1 goto :error
echo   asio: OK
goto :step2

:asio_ok
echo   asio: already exists

:step2
echo.
echo ================================================================
echo  [Step 2] Running CMake configure...
echo ================================================================
cd /d "%PLUGIN_DIR%"

if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

set OBS_PLUGIN_LEGACY_INSTALL=OFF
if "%OBS_LEGACY_INSTALL%"=="1" set OBS_PLUGIN_LEGACY_INSTALL=ON

cmake -S "%PLUGIN_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DOBS_SOURCE_DIR="%OBS_SOURCE_DIR%" -DOBS_PLUGIN_LEGACY_INSTALL=%OBS_PLUGIN_LEGACY_INSTALL%
if errorlevel 1 (
    echo.
    echo [ERROR] CMake configure failed.
    echo  OBS_SOURCE_DIR = %OBS_SOURCE_DIR%
    goto :error
)
echo   CMake configure: OK

echo.
echo ================================================================
echo  [Step 3] Building plugin...
echo ================================================================
cmake --build "%BUILD_DIR%" --config RelWithDebInfo --parallel
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed.
    goto :error
)

if not exist "%BUILD_DIR%\RelWithDebInfo\obs-delay-stream.dll" (
    echo [ERROR] DLL was not generated.
    goto :error
)
echo   Build OK: %BUILD_DIR%\RelWithDebInfo\obs-delay-stream.dll

echo.
echo ================================================================
echo  [Step 4] Installing to OBS...
echo ================================================================

if "%CI_MODE%"=="1" goto :skip_install

if "%OBS_LEGACY_INSTALL%"=="1" goto :legacy_paths
set PLUGIN_DEST=%OBS_INSTALL_DIR%\plugins\obs-delay-stream\bin\64bit
set LOCALE_DEST=%OBS_INSTALL_DIR%\plugins\obs-delay-stream\data\locale
set RECEIVER_DEST=%OBS_INSTALL_DIR%\plugins\obs-delay-stream\data\receiver
goto :paths_set
:legacy_paths
set PLUGIN_DEST=%OBS_INSTALL_DIR%\obs-plugins\64bit
set LOCALE_DEST=%OBS_INSTALL_DIR%\data\obs-plugins\obs-delay-stream\locale
set RECEIVER_DEST=%OBS_INSTALL_DIR%\data\obs-plugins\obs-delay-stream\receiver
:paths_set

if not exist "%PLUGIN_DEST%" mkdir "%PLUGIN_DEST%"
if not exist "%LOCALE_DEST%" mkdir "%LOCALE_DEST%"
if not exist "%RECEIVER_DEST%" mkdir "%RECEIVER_DEST%"

copy /Y "%BUILD_DIR%\RelWithDebInfo\obs-delay-stream.dll" "%PLUGIN_DEST%\"
if errorlevel 1 (
    echo [ERROR] Failed to copy DLL. Run as Administrator.
    goto :error
)

copy /Y "%PLUGIN_DIR%\data\locale\*.ini" "%LOCALE_DEST%\"
if errorlevel 1 (
    echo [ERROR] Failed to copy locale files.
    goto :error
)

if not exist "%RECEIVER_BUILD_DIR%\index.html" (
    echo [ERROR] Missing receiver build output: %RECEIVER_BUILD_DIR%\index.html
    echo         Re-run the build and check receiver npm build logs.
    goto :error
)

xcopy /E /I /Y "%RECEIVER_BUILD_DIR%" "%RECEIVER_DEST%\" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy receiver build output.
    goto :error
)
if not exist "%RECEIVER_DEST%\third_party\i18next\i18next.min.js" (
    echo [ERROR] Failed to deploy receiver asset: third_party\i18next\i18next.min.js
    goto :error
)
if not exist "%RECEIVER_DEST%\third_party\i18next\LICENSE" (
    echo [ERROR] Failed to deploy receiver asset: third_party\i18next\LICENSE
    goto :error
)
if not exist "%RECEIVER_DEST%\index.html" (
    echo [ERROR] Failed to deploy receiver asset: index.html
    goto :error
)

echo.
echo ================================================================
echo  SUCCESS: Build and install complete!
echo ================================================================
echo.
echo  Install dir: %OBS_INSTALL_DIR%\plugins\obs-delay-stream
echo.
echo  1. Open OBS Studio
echo  2. Right-click audio source - Filters - click +
echo  3. Select "obs-delay-stream"
echo.
pause
exit /b 0

:receiver_only
echo.
echo ================================================================
echo  [Receiver Only] Installing receiver files...
echo ================================================================
echo.

if "%OBS_LEGACY_INSTALL%"=="1" goto :receiver_legacy_paths
set RECEIVER_DEST=%OBS_INSTALL_DIR%\plugins\obs-delay-stream\data\receiver
goto :receiver_paths_set
:receiver_legacy_paths
set RECEIVER_DEST=%OBS_INSTALL_DIR%\data\obs-plugins\obs-delay-stream\receiver
:receiver_paths_set

if not exist "%RECEIVER_DEST%" mkdir "%RECEIVER_DEST%"

call :build_receiver_assets
if errorlevel 1 goto :error

xcopy /E /I /Y "%RECEIVER_BUILD_DIR%" "%RECEIVER_DEST%\" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy receiver build output.
    goto :error
)
if not exist "%RECEIVER_DEST%\third_party\i18next\i18next.min.js" (
    echo [ERROR] Failed to deploy receiver asset: third_party\i18next\i18next.min.js
    goto :error
)
if not exist "%RECEIVER_DEST%\third_party\i18next\LICENSE" (
    echo [ERROR] Failed to deploy receiver asset: third_party\i18next\LICENSE
    goto :error
)
if not exist "%RECEIVER_DEST%\index.html" (
    echo [ERROR] Failed to deploy receiver asset: index.html
    goto :error
)

echo.
echo ================================================================
echo  SUCCESS: Receiver files installed.
echo ================================================================
echo.
echo  Receiver : %RECEIVER_DEST%
echo.
if "%CI_MODE%"=="1" exit /b 0
pause
exit /b 0

:build_receiver_assets
echo.
echo ================================================================
echo  [Receiver Build] Building receiver assets with npm...
echo ================================================================
echo.

where npm >nul 2>&1
if errorlevel 1 (
    echo [ERROR] npm not found. Install Node.js 20+ from: https://nodejs.org/
    exit /b 1
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if exist "%RECEIVER_BUILD_DIR%" rmdir /s /q "%RECEIVER_BUILD_DIR%"

set "RECEIVER_BUILD_OUT_DIR=%RECEIVER_BUILD_DIR%"
set "RECEIVER_PROJECT_VERSION=%PLUGIN_VERSION%"
pushd "%PLUGIN_DIR%\receiver"
npm run build
set "RECEIVER_BUILD_RC=%ERRORLEVEL%"
popd
set "RECEIVER_BUILD_OUT_DIR="
set "RECEIVER_PROJECT_VERSION="

if not "%RECEIVER_BUILD_RC%"=="0" (
    echo [ERROR] Receiver npm build failed.
    exit /b 1
)
if not exist "%RECEIVER_BUILD_DIR%\index.html" (
    echo [ERROR] Receiver build output missing: %RECEIVER_BUILD_DIR%\index.html
    exit /b 1
)

echo   Receiver build OK: %RECEIVER_BUILD_DIR%
exit /b 0

:skip_install
echo.
echo ================================================================
echo  SUCCESS: Build complete (CI mode, install skipped).
echo ================================================================
echo.
if "%CI_MODE%"=="1" exit /b 0
pause
exit /b 0

:error
echo.
echo ================================================================
echo  ERROR: Build failed.
echo ================================================================
echo.
if "%CI_MODE%"=="1" exit /b 1
pause
exit /b 1
