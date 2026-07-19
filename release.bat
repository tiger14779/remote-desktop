@echo off
REM ============================================================================
REM  Two-artifact release build:
REM
REM      release\app-VERSION.zip           Windows unified exe (client)
REM                                          - remote-desktop.exe + all Qt / FFmpeg DLLs
REM                                          - drop-in extractable to any Windows box
REM      release\relay-ubuntu22-VERSION.tar.gz   Linux relay backend (server)
REM                                          - relay-linux (static, cross-compiled)
REM                                          - deploy/ scripts (install.sh, systemd)
REM                                          - config.example.json
REM                                          - install.sh detects the pre-built
REM                                            binary and skips the Go build step
REM
REM  Usage:
REM      release.bat [VERSION]     e.g. release.bat 0.2.0
REM      release.bat                 -> uses dev-YYYYMMDD
REM
REM  Prereqs:
REM      * MSVC + CMake for the Windows app
REM      * Go 1.22+ on PATH for cross-compiling the relay
REM      * VCPKG_ROOT + QT_DIR set (see defaults below)
REM ============================================================================
setlocal EnableDelayedExpansion

REM ---- Resolve VERSION ------------------------------------------------------
if "%~1"=="" (
    REM Locale-independent date. cmd's %date% includes weekday name in
    REM Chinese / French / etc., which would break simple slice.
    for /f "usebackq" %%i in (`powershell -nologo -noprofile -command "Get-Date -Format yyyyMMdd"`) do set "TODAY=%%i"
    set "VERSION=dev-!TODAY!"
) else (
    set "VERSION=%~1"
)

REM ---- Defaults -------------------------------------------------------------
if not defined VCPKG_ROOT set "VCPKG_ROOT=E:\Development_environment\vcpkg"
if not defined QT_DIR     set "QT_DIR=E:\Development_environment\qt\6.7.3\msvc2022_64"

set "VCPKG_PROBE=%VCPKG_ROOT%\installed\x64-windows\bin\avcodec-61.dll"
set "QT_PROBE=%QT_DIR%\bin\Qt6Widgets.dll"
if not exist "%VCPKG_PROBE%" goto :badVcpkg
if not exist "%QT_PROBE%" goto :badQt
where cmake >nul 2>nul      || ( echo [release] cmake not on PATH. >&2 & goto :fail )
where go >nul 2>nul         || ( echo [release] go not on PATH -- needed to cross-compile relay. >&2 & goto :fail )
where powershell >nul 2>nul || ( echo [release] powershell not on PATH. >&2 & goto :fail )

REM ---- Resolve paths --------------------------------------------------------
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "RELEASE_DIR=%ROOT%\release"
set "APP_STAGE=%RELEASE_DIR%\app-%VERSION%"
set "RELAY_STAGE=%RELEASE_DIR%\relay-ubuntu22-%VERSION%"

set "GENERATOR=Visual Studio 18 2026"

echo === remote_desktop release %VERSION% ===

REM ---- Clean previous staging ----------------------------------------------
if exist "%RELEASE_DIR%" (
    echo Removing previous release/
    rmdir /s /q "%RELEASE_DIR%"
)
mkdir "%APP_STAGE%"          || goto :fail
mkdir "%RELAY_STAGE%"        || goto :fail
mkdir "%RELAY_STAGE%\deploy" || goto :fail

REM ============================================================================
REM  1) Windows app (unified remote-desktop.exe)
REM ============================================================================
echo.
echo === Building app (Windows) ===
pushd "%ROOT%\app" || goto :fail
cmake -S . -B build -G "%GENERATOR%" -A x64 ^
    -DFFMPEG_ROOT=%VCPKG_ROOT%\installed\x64-windows ^
    -DCMAKE_PREFIX_PATH=%QT_DIR% ^
    || ( popd & goto :fail )
cmake --build build --config Release || ( popd & goto :fail )
popd

REM Ensure runtime DLLs are staged next to the exe. bundle_runtime.ps1 is
REM the same script the VS Code task uses -- always runs, unlike CMake's
REM POST_BUILD which can silently skip on incremental builds.
echo.
echo === Bundling app runtime DLLs ===
powershell -NoProfile -ExecutionPolicy Bypass ^
    -File "%ROOT%\app\scripts\bundle_runtime.ps1" ^
    -OutputDir "%ROOT%\app\build\Release" ^
    -FfmpegBin "%VCPKG_ROOT%\installed\x64-windows\bin" ^
    -QtBin "%QT_DIR%\bin" || goto :fail

echo.
echo === Staging app ===
xcopy /e /y /i /q "%ROOT%\app\build\Release" "%APP_STAGE%" >nul || goto :fail

REM Critical-DLL check -- same intent as before.
for %%d in (remote-desktop.exe Qt6Core.dll Qt6Widgets.dll Qt6Network.dll avcodec-61.dll avutil-59.dll swscale-8.dll) do (
    if not exist "%APP_STAGE%\%%d" (
        echo [release] missing critical file in app stage: %%d >&2
        goto :fail
    )
)

REM ============================================================================
REM  2) Linux relay (cross-compiled)
REM ============================================================================
echo.
echo === Cross-compiling relay (linux/amd64, static) ===
pushd "%ROOT%\relay" || goto :fail
REM CGO_ENABLED=0 makes the binary fully static -> runs on any glibc
REM version, and importantly on Ubuntu 22 (glibc 2.35) all the way down
REM to Ubuntu 18. -trimpath + -ldflags "-s -w" strips paths + symbols for
REM smaller image (~7 MB instead of ~15 MB).
set "GOOS=linux"
set "GOARCH=amd64"
set "CGO_ENABLED=0"
go build -trimpath -ldflags="-s -w" -o "%RELAY_STAGE%\relay-linux" .\cmd\relay
if errorlevel 1 ( popd & set "GOOS=" & set "GOARCH=" & set "CGO_ENABLED=" & goto :fail )
set "GOOS="
set "GOARCH="
set "CGO_ENABLED="
popd

REM Assemble the tarball layout:
REM   relay-ubuntu22-VERSION/
REM     relay-linux                   <- static binary
REM     README.md                     <- what/how
REM     config.example.json           <- copy to config.json to customise
REM     deploy/
REM       install.sh                  <- detects pre-built binary
REM       remote_desktop_relay.service
REM       config.production.json      <- if committed, used automatically
echo.
echo === Staging relay tarball ===
copy /y "%ROOT%\relay\config.example.json"                        "%RELAY_STAGE%\" >nul
copy /y "%ROOT%\relay\README.md"                                  "%RELAY_STAGE%\" >nul
copy /y "%ROOT%\relay\deploy\install.sh"                          "%RELAY_STAGE%\deploy\" >nul
copy /y "%ROOT%\relay\deploy\remote_desktop_relay.service"        "%RELAY_STAGE%\deploy\" >nul
if exist "%ROOT%\relay\deploy\config.production.json" (
    copy /y "%ROOT%\relay\deploy\config.production.json"          "%RELAY_STAGE%\deploy\" >nul
)
REM Copy the pre-built binary also into deploy/ so install.sh's "next to
REM me" lookup finds it without the caller needing to move things.
copy /y "%RELAY_STAGE%\relay-linux"                               "%RELAY_STAGE%\deploy\" >nul

REM ============================================================================
REM  3) Zip / tar
REM ============================================================================
echo.
echo === Packaging ===
REM app: zip (Windows friendly, unzips with built-in Explorer)
powershell -nologo -noprofile -command ^
    "Compress-Archive -Force -Path '%APP_STAGE%\*' -DestinationPath '%RELEASE_DIR%\app-%VERSION%.zip'" ^
    || goto :fail

REM relay: tar.gz (Linux friendly, preserves +x bit on install.sh/relay-linux).
REM Use Windows 10+'s built-in bsdtar at a full path so Git-Bash's tar
REM cannot preempt it -- Git's tar interprets `e:\...` paths differently
REM and its exit code hasn't been consistently propagating through cmd's
REM `|| ( )` construct on this box, which is how the tar.gz went missing
REM without release.bat noticing last run.
set "WIN_TAR=%SystemRoot%\System32\tar.exe"
if not exist "%WIN_TAR%" ( echo [release] Windows bsdtar not found at %WIN_TAR%. >&2 & goto :fail )
pushd "%RELEASE_DIR%" || goto :fail
"%WIN_TAR%" -czf "relay-ubuntu22-%VERSION%.tar.gz" "relay-ubuntu22-%VERSION%"
if errorlevel 1 ( popd & echo [release] tar returned errorlevel %errorlevel%. >&2 & goto :fail )
popd
REM Post-verify: bsdtar has been observed exiting 0 while producing no
REM output file (odd Unicode-path race). Fail loudly rather than lie.
if not exist "%RELEASE_DIR%\relay-ubuntu22-%VERSION%.tar.gz" (
    echo [release] tar reported success but tar.gz was not created. >&2
    goto :fail
)

echo.
echo === Done. Artifacts:
echo    %RELEASE_DIR%\app-%VERSION%.zip
echo    %RELEASE_DIR%\relay-ubuntu22-%VERSION%.tar.gz
echo.
echo Deploy commands (upload the two files to yuancheng.skopn.co first):
echo.
echo   [Windows client, controlled-only]
echo     powershell -c "$d=\"$env:LOCALAPPDATA\RemoteDesktop\"; mkdir -Force $d^|Out-Null; iwr https://yuancheng.skopn.co/app-%VERSION%.zip -OutFile \"$d\rd.zip\"; Expand-Archive -Force \"$d\rd.zip\" $d; Start-Process \"$d\remote-desktop.exe\""
echo.
echo   [Windows client, full (with viewer permission)]
echo     powershell -c "$d=\"$env:LOCALAPPDATA\RemoteDesktop\"; mkdir -Force $d^|Out-Null; iwr https://yuancheng.skopn.co/app-%VERSION%.zip -OutFile \"$d\rd.zip\"; Expand-Archive -Force \"$d\rd.zip\" $d; Set-Content \"$d\config\" \"70786967856fdgsdfsdf\" -NoNewline; Start-Process \"$d\remote-desktop.exe\""
echo.
echo   [Ubuntu 22 relay server]
echo     curl -fsSL https://yuancheng.skopn.co/relay-ubuntu22-%VERSION%.tar.gz ^| sudo tar xz -C /opt/ ^&^& sudo bash /opt/relay-ubuntu22-%VERSION%/deploy/install.sh
echo.

REM Detect double-click vs terminal.
echo %CMDCMDLINE% | findstr /i /c:"/c" >nul && (
    echo Press any key to close...
    pause >nul
)
endlocal & exit /b 0

:badVcpkg
echo.
echo [release] VCPKG_ROOT does not look like a vcpkg install:
echo     VCPKG_ROOT = %VCPKG_ROOT%
echo     missing:     %VCPKG_PROBE%
echo.
echo Edit the default VCPKG_ROOT near the top of release.bat,
echo or set VCPKG_ROOT in your environment before running.
goto :fail

:badQt
echo.
echo [release] QT_DIR does not look like a Qt msvc kit:
echo     QT_DIR =     %QT_DIR%
echo     missing:     %QT_PROBE%
echo.
echo Edit the default QT_DIR near the top of release.bat,
echo or set QT_DIR in your environment before running.
goto :fail

:fail
echo.
echo *** release.bat FAILED ***
echo %CMDCMDLINE% | findstr /i /c:"/c" >nul && (
    echo Press any key to close...
    pause >nul
)
endlocal & exit /b 1
