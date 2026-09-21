@echo off
rem Build BoTapMusic (MusicPlayer2.sln).
rem
rem Usage (run from anywhere; the script switches to the repo root by itself):
rem   scripts\build.bat                 Release x86 (default, the verified target)
rem   scripts\build.bat Debug           config only, platform defaults to x86
rem   scripts\build.bat Release x64
rem
rem MSVC is located in this order:
rem   1. VSINSTALL (or VSBUILDTOOLS) environment variable - set it to override
rem   2. vswhere.exe - available when the Visual Studio Installer is installed
rem   3. D:\VSBuildTools - the build tools on this machine, not registered with vswhere
rem
rem Cleaning intermediate files is a separate job: bash scripts/clean-build.sh
rem Note: this file must keep CRLF line endings, cmd cannot parse it with LF alone.

setlocal

set "CONFIG=%~1"
set "PLATFORM=%~2"
if "%CONFIG%"=="" set "CONFIG=Release"
if "%PLATFORM%"=="" set "PLATFORM=x86"

pushd "%~dp0.."
set "ROOT=%CD%"

set "VS_DIR=%VSINSTALL%"
if "%VS_DIR%"=="" set "VS_DIR=%VSBUILDTOOLS%"
if not "%VS_DIR%"=="" goto have_vs

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto try_default
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VS_DIR=%%i"
if not "%VS_DIR%"=="" goto have_vs

:try_default
if exist "D:\VSBuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VS_DIR=D:\VSBuildTools"
if "%VS_DIR%"=="" (
    echo Cannot find MSVC. Set VSINSTALL to the VS / build tools directory, for example:
    echo     set VSINSTALL=D:\VSBuildTools
    echo     scripts\build.bat
    popd
    exit /b 1
)

:have_vs
set "VCVARS=%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat"
set "MSBUILD_EXE=%VS_DIR%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD_EXE%" set "MSBUILD_EXE=%VS_DIR%\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not exist "%VCVARS%" (
    echo No VC\Auxiliary\Build\vcvarsall.bat under %VS_DIR% - does not look like a VS install dir.
    popd
    exit /b 1
)
if not exist "%MSBUILD_EXE%" (
    echo MSBuild.exe not found: %MSBUILD_EXE%
    popd
    exit /b 1
)

echo Using %VS_DIR%
echo Building %CONFIG% %PLATFORM% ...
call "%VCVARS%" %PLATFORM%
"%MSBUILD_EXE%" MusicPlayer2.sln /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /m /nologo /v:m
if errorlevel 1 (
    echo.
    echo Build FAILED. If the error is LNK1104, the player is probably running ^(single instance^) - close it and retry.
    popd
    exit /b 1
)

if /i "%PLATFORM%"=="x64" (set "OUT=x64\%CONFIG%") else (set "OUT=%CONFIG%")
echo.
echo Done: %ROOT%\%OUT%\MusicPlayer2.exe
popd
endlocal
