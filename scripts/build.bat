@echo off
rem 构建 BoTapMusic（MusicPlayer2.sln）。
rem
rem 用法（在仓库根目录或任意位置都可以，脚本自己会切到仓库根）：
rem   scripts\build.bat                  REM Release x86（已验证的构建目标）
rem   scripts\build.bat Debug            REM 只给配置，平台默认 x86
rem   scripts\build.bat Release x64
rem
rem MSVC 按这个顺序找：
rem   1. 环境变量 VSINSTALL（或 VSBUILDTOOLS），想指定就用它
rem   2. vswhere，装过 Visual Studio Installer 就有
rem   3. D:\VSBuildTools，本机那套生成工具没注册到 vswhere，只能写死兜底
rem
rem 清理中间产物是另一件事：bash scripts/clean-build.sh

setlocal

set "CONFIG=%~1"
set "PLATFORM=%~2"
if "%CONFIG%"=="" set "CONFIG=Release"
if "%PLATFORM%"=="" set "PLATFORM=x86"

pushd "%~dp0.."
set "ROOT=%CD%"

set "VSINSTALL=%VSINSTALL%"
if "%VSINSTALL%"=="" set "VSINSTALL=%VSBUILDTOOLS%"
if not "%VSINSTALL%"=="" goto :have_vs

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
)
if not "%VSINSTALL%"=="" goto :have_vs

if exist "D:\VSBuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VSINSTALL=D:\VSBuildTools"
if "%VSINSTALL%"=="" (
    echo 找不到 MSVC。请先设置 VSINSTALL 指向 VS 或生成工具的安装目录，例如：
    echo     set VSINSTALL=D:\VSBuildTools
    echo     scripts\build.bat
    popd
    exit /b 1
)

:have_vs
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
set "MSBUILD=%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" set "MSBUILD=%VSINSTALL%\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not exist "%VCVARS%" (
    echo %VSINSTALL% 里没有 VC\Auxiliary\Build\vcvarsall.bat，不像 VS 的安装目录。
    popd
    exit /b 1
)
if not exist "%MSBUILD%" (
    echo 找不到 MSBuild.exe：%MSBUILD%
    popd
    exit /b 1
)

echo 使用 %VSINSTALL%
echo 构建 %CONFIG% %PLATFORM% ...
call "%VCVARS%" %PLATFORM%
"%MSBUILD%" MusicPlayer2.sln /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /m /nologo /v:m
if errorlevel 1 (
    echo.
    echo 编译失败。如果报错是 LNK1104，多半是播放器正在运行（单实例），关掉再来一次。
    popd
    exit /b 1
)

if /i "%PLATFORM%"=="x64" (set "OUT=x64\%CONFIG%") else (set "OUT=%CONFIG%")
echo.
echo 完成：%ROOT%\%OUT%\MusicPlayer2.exe
popd
endlocal
