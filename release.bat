@echo off
REM ============================================================================
REM  Build agent + viewer in Release, stage them with all required runtime DLLs,
REM  and pack each into a zip for distribution.
REM
REM  Usage:
REM      release.bat [VERSION]      e.g. release.bat 0.1.0
REM      release.bat                  -> uses dev-YYYYMMDD
REM
REM  Required env vars:
REM      VCPKG_ROOT  - vcpkg root (must contain installed\x64-windows\bin with
REM                    avcodec*.dll, avutil*.dll, swscale*.dll)
REM      QT_DIR      - Qt msvc kit dir (e.g. ...\qt\6.7.3\msvc2022_64)
REM
REM  Output: release\agent-VERSION.zip, release\viewer-VERSION.zip
REM ============================================================================
setlocal EnableDelayedExpansion

REM ---- Resolve VERSION ------------------------------------------------------
if "%~1"=="" (
    REM %date% format depends on locale; try the ISO-ish slice first, fall back
    REM to a timestamp call. Both produce e.g. dev-20260617.
    set "TODAY=%date:~0,4%%date:~5,2%%date:~8,2%"
    set "VERSION=dev-!TODAY!"
) else (
    set "VERSION=%~1"
)

REM ---- Sanity check env -----------------------------------------------------
if not defined VCPKG_ROOT (
    echo [release] VCPKG_ROOT is not set. >&2
    exit /b 1
)
if not defined QT_DIR (
    echo [release] QT_DIR is not set. >&2
    exit /b 1
)
where cmake >nul 2>nul || ( echo [release] cmake not on PATH. >&2 & exit /b 1 )
where powershell >nul 2>nul || ( echo [release] powershell not on PATH. >&2 & exit /b 1 )

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
mkdir "%AGENT_STAGE%"  || exit /b 1
mkdir "%VIEWER_STAGE%" || exit /b 1

REM ---- Build agent ----------------------------------------------------------
echo.
echo === Building agent ===
pushd "%ROOT%\agent" || exit /b 1
cmake -S . -B build -G "%GENERATOR%" -A x64 || ( popd & exit /b 2 )
cmake --build build --config Release -- /m /v:minimal || ( popd & exit /b 2 )
popd

REM ---- Build viewer ---------------------------------------------------------
echo.
echo === Building viewer ===
pushd "%ROOT%\viewer" || exit /b 1
cmake -S . -B build -G "%GENERATOR%" -A x64 || ( popd & exit /b 3 )
cmake --build build --config Release -- /m /v:minimal || ( popd & exit /b 3 )
popd

REM ---- Stage agent ----------------------------------------------------------
echo.
echo === Staging agent ===
set "AGENT_BUILD=%ROOT%\agent\build\Release"
copy /y "%AGENT_BUILD%\agent.exe"        "%AGENT_STAGE%\" >nul || exit /b 4
copy /y "%AGENT_BUILD%\capture_test.exe" "%AGENT_STAGE%\" >nul
copy /y "%AGENT_BUILD%\encode_test.exe"  "%AGENT_STAGE%\" >nul
REM FFmpeg + transitive deps were copied to the build dir by CMake's POST_BUILD.
for %%f in ("%AGENT_BUILD%\*.dll") do copy /y "%%f" "%AGENT_STAGE%\" >nul
copy /y "%ROOT%\agent\README.md"         "%AGENT_STAGE%\README.md" >nul

REM ---- Stage viewer (Qt + FFmpeg + plugin subdirs) --------------------------
echo === Staging viewer ===
set "VIEWER_BUILD=%ROOT%\viewer\build\Release"
copy /y "%VIEWER_BUILD%\viewer.exe"      "%VIEWER_STAGE%\" >nul || exit /b 5
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
    "Compress-Archive -Force -Path '%AGENT_STAGE%'  -DestinationPath '%RELEASE_DIR%\agent-%VERSION%.zip'"  || exit /b 6
powershell -nologo -noprofile -command ^
    "Compress-Archive -Force -Path '%VIEWER_STAGE%' -DestinationPath '%RELEASE_DIR%\viewer-%VERSION%.zip'" || exit /b 6

echo.
echo === Done. Artifacts:
for %%z in ("%RELEASE_DIR%\*.zip") do echo    %%~nxz  ^(%%~zz bytes^)

endlocal & exit /b 0
