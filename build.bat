@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 437 > nul
title obs-delay-stream Build Script

echo ================================================================
echo  obs-delay-stream v1.7.0  Build Script
echo ================================================================
echo.

set PLUGIN_DIR=C:\CC\obs-delay-stream
set OBS_SOURCE_DIR=C:\obs-studio
set OBS_INSTALL_DIR=C:\Program Files\obs-studio
set BUILD_DIR=%PLUGIN_DIR%\build

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
if not exist "%OBS_SOURCE_DIR%\CMakeLists.txt" (
    echo   Cloning OBS Studio...
    git clone --recursive https://github.com/obsproject/obs-studio.git "%OBS_SOURCE_DIR%"
    if errorlevel 1 goto :error
)
cd /d "%OBS_SOURCE_DIR%"
cmake -S . -B build_x64 -G "Visual Studio 17 2022" -A x64 -DENABLE_BROWSER=OFF -DENABLE_VST=OFF -DENABLE_SCRIPTING=OFF -DENABLE_PLUGINS=OFF -DENABLE_UI=OFF
if errorlevel 1 goto :error
cmake --build build_x64 --config RelWithDebInfo --target libobs obs-frontend-api --parallel
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

cmake -S "%PLUGIN_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DOBS_SOURCE_DIR="%OBS_SOURCE_DIR%"
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

set PLUGIN_DEST=%OBS_INSTALL_DIR%\obs-plugins\64bit
set LOCALE_DEST=%OBS_INSTALL_DIR%\data\obs-plugins\obs-delay-stream\locale

if not exist "%PLUGIN_DEST%" mkdir "%PLUGIN_DEST%"
if not exist "%LOCALE_DEST%" mkdir "%LOCALE_DEST%"

copy /Y "%BUILD_DIR%\RelWithDebInfo\obs-delay-stream.dll" "%PLUGIN_DEST%\"
if errorlevel 1 (
    echo [ERROR] Failed to copy DLL. Run as Administrator.
    goto :error
)

copy /Y "%PLUGIN_DIR%\data\locale\en-US.ini" "%LOCALE_DEST%\"
if errorlevel 1 (
    echo [ERROR] Failed to copy locale file.
    goto :error
)

:: Install port release helper script
copy /Y "%PLUGIN_DIR%\release_port.bat" "%OBS_INSTALL_DIR%\release_port.bat" >nul 2>&1

echo.
echo ================================================================
echo  SUCCESS: Build and install complete!
echo ================================================================
echo.
echo  DLL : %PLUGIN_DEST%\obs-delay-stream.dll
echo  INI : %LOCALE_DEST%\en-US.ini
echo.
echo  1. Open OBS Studio
echo  2. Right-click audio source - Filters - click +
echo  3. Select "Delay Stream (delay + WebSocket)"
echo.
pause
exit /b 0

:error
echo.
echo ================================================================
echo  ERROR: Build failed.
echo ================================================================
echo.
pause
exit /b 1
