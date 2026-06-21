@echo off
REM ============================================================================
REM  Build agent + viewer in Release, stage them with all required runtime DLLs,
REM  and pack each into a zip for distribution.
REM
REM  Usage:
REM      release.bat [VERSION]      e.g. release.bat 0.1.0
REM      release.bat                  -> uses dev-YYYYMMDD
REM
REM  Paths to vcpkg + Qt are baked in below as defaults — see "Defaults"
REM  section. Override with env vars only if your install lives elsewhere:
REM      set VCPKG_ROOT=D:\my\vcpkg & release.bat
REM
REM  Output: release\agent-VERSION.zip, release\viewer-VERSION.zip
REM ============================================================================
setlocal EnableDelayedExpansion

REM ---- Resolve VERSION ------------------------------------------------------
if "%~1"=="" (
    REM Locale-independent date. cmd's %date% includes the weekday name in
    REM Chinese / French / etc., which would break a simple slice.
    for /f "usebackq" %%i in (`powershell -nologo -noprofile -command "Get-Date -Format yyyyMMdd"`) do set "TODAY=%%i"
    set "VERSION=dev-!TODAY!"
) else (
    set "VERSION=%~1"
)

REM ---- Defaults (edit these once, no env vars needed) -----------------------
REM Hardcoded paths for the standard dev machine. They're only used when the
REM env vars aren't already set — so CI / another machine with different
REM layout can still override with `set VCPKG_ROOT=... & release.bat`.
if not defined VCPKG_ROOT set "VCPKG_ROOT=E:\Development_environment\vcpkg"
if not defined QT_DIR     set "QT_DIR=E:\Development_environment\qt\6.7.3\msvc2022_64"

REM ---- Validate the resolved paths actually point at something --------------
REM Note: keep the if-block bodies short and parens-free — cmd's parser gets
REM weird with literal parentheses inside if-block bodies even when balanced.
set "VCPKG_PROBE=%VCPKG_ROOT%\installed\x64-windows\bin\avcodec-61.dll"
set "QT_PROBE=%QT_DIR%\bin\Qt6Widgets.dll"
if not exist "%VCPKG_PROBE%" goto :badVcpkg
if not exist "%QT_PROBE%" goto :badQt
where cmake >nul 2>nul || ( echo [release] cmake not on PATH. >&2 & goto :fail )
where powershell >nul 2>nul || ( echo [release] powershell not on PATH. >&2 & goto :fail )

REM ---- Resolve paths --------------------------------------------------------
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "RELEASE_DIR=%ROOT%\release"
set "AGENT_STAGE=%RELEASE_DIR%\agent-%VERSION%"
set "VIEWER_STAGE=%RELEASE_DIR%\viewer-%VERSION%"

set "GENERATOR=Visual Studio 18 2026"

echo === remote_desktop release %VERSION% ===

REM ---- Clean previous staging ----------------------------------------------
if exist "%RELEASE_DIR%" (
    echo Removing previous release/
    rmdir /s /q "%RELEASE_DIR%"
)
mkdir "%AGENT_STAGE%"  || goto :fail
mkdir "%VIEWER_STAGE%" || goto :fail

REM ---- Build agent ----------------------------------------------------------
echo.
echo === Building agent ===
pushd "%ROOT%\agent" || goto :fail
cmake -S . -B build -G "%GENERATOR%" -A x64 || ( popd & goto :fail )
cmake --build build --config Release -- /m /v:minimal || ( popd & goto :fail )
popd

REM ---- Build viewer ---------------------------------------------------------
echo.
echo === Building viewer ===
pushd "%ROOT%\viewer" || goto :fail
cmake -S . -B build -G "%GENERATOR%" -A x64 || ( popd & goto :fail )
cmake --build build --config Release -- /m /v:minimal || ( popd & goto :fail )
popd

REM ---- Stage agent ----------------------------------------------------------
echo.
echo === Staging agent ===
set "AGENT_BUILD=%ROOT%\agent\build\Release"
copy /y "%AGENT_BUILD%\agent.exe"        "%AGENT_STAGE%\" >nul || goto :fail
copy /y "%AGENT_BUILD%\capture_test.exe" "%AGENT_STAGE%\" >nul
copy /y "%AGENT_BUILD%\encode_test.exe"  "%AGENT_STAGE%\" >nul
REM FFmpeg + transitive deps were copied to the build dir by CMake's POST_BUILD.
for %%f in ("%AGENT_BUILD%\*.dll") do copy /y "%%f" "%AGENT_STAGE%\" >nul
copy /y "%ROOT%\agent\README.md"         "%AGENT_STAGE%\README.md" >nul

REM Critical-DLL check: CMake's POST_BUILD copy_directory has been seen to
REM silently miss avcodec/avutil/swscale when build/Release was locked at
REM the time of an earlier build. Catch it here instead of at the customer.
for %%d in (avcodec-61.dll avutil-59.dll swscale-8.dll swresample-5.dll libx264-164.dll vcruntime140.dll) do (
    if not exist "%AGENT_STAGE%\%%d" (
        echo [release] missing critical DLL in agent stage: %%d >&2
        echo            ^(rerun cmake --build agent or copy from vcpkg bin^) >&2
        goto :fail
    )
)

REM ---- Stage viewer (Qt + FFmpeg + plugin subdirs) --------------------------
echo === Staging viewer ===
set "VIEWER_BUILD=%ROOT%\viewer\build\Release"
copy /y "%VIEWER_BUILD%\viewer.exe"      "%VIEWER_STAGE%\" >nul || goto :fail
copy /y "%VIEWER_BUILD%\decode_test.exe" "%VIEWER_STAGE%\" >nul
for %%f in ("%VIEWER_BUILD%\*.dll") do copy /y "%%f" "%VIEWER_STAGE%\" >nul
REM Qt plugin subdirectories — windeployqt put them here. Copy whatever is present.
for %%d in (platforms styles imageformats iconengines tls generic) do (
    if exist "%VIEWER_BUILD%\%%d" (
        xcopy /e /y /i /q "%VIEWER_BUILD%\%%d" "%VIEWER_STAGE%\%%d" >nul
    )
)
copy /y "%ROOT%\viewer\README.md" "%VIEWER_STAGE%\README.md" >nul

REM ---- Zip up ---------------------------------------------------------------
echo.
echo === Zipping ===
powershell -nologo -noprofile -command ^
    "Compress-Archive -Force -Path '%AGENT_STAGE%'  -DestinationPath '%RELEASE_DIR%\agent-%VERSION%.zip'"  || goto :fail
powershell -nologo -noprofile -command ^
    "Compress-Archive -Force -Path '%VIEWER_STAGE%' -DestinationPath '%RELEASE_DIR%\viewer-%VERSION%.zip'" || goto :fail

echo.
echo === Done. Artifacts:
for %%z in ("%RELEASE_DIR%\*.zip") do echo    %%~nxz  ^(%%~zz bytes^)
echo.

REM Detect double-click vs terminal: when launched from explorer, %CMDCMDLINE%
REM is something like "C:\WINDOWS\system32\cmd.exe /c ""...release.bat""".
REM Pause only in that case so terminal users don't get an extra keystroke.
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
