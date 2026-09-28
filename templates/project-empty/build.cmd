@echo off
rem ===========================================================================
rem WorldEngine project build (PROJ-8/T2): builds this project's own Game.dll.
rem
rem Output: project\build\x64-CONFIG\bin\CONFIG\Game\CONFIG\Game.dll
rem         plus WorldRuntime.dll - the engine runtime - next to it.
rem
rem Engine root resolution order:
rem   1) first argument: build.cmd ENGINE_ROOT CONFIG
rem   2) environment variable WE_ROOT
rem   3) the engine root recorded in the wizard's "project-Edit.cmd" / "-Play.cmd"
rem   4) ..\.. - the template lives inside the engine checkout under templates\
rem
rem Usage: build.cmd
rem        build.cmd E:\WorldEngine
rem        build.cmd E:\WorldEngine Release
rem ===========================================================================
setlocal EnableExtensions
set "WE_CONFIG=%~2"
if "%WE_CONFIG%"=="" set "WE_CONFIG=Debug"

if not "%~1"=="" set "WE_ROOT=%~1"
if not defined WE_ROOT call :read_launcher_root
if not defined WE_ROOT call :guess_relative_root

if not defined WE_ROOT (
    echo [error] WorldEngine engine root not found.
    echo         Usage: build.cmd ENGINE_ROOT [Debug or Release]
    echo         Or set WE_ROOT to the checkout that owns CMakeLists.txt, Engine and Game.
    exit /b 2
)
if not exist "%WE_ROOT%\CMakeLists.txt" (
    echo [error] "%WE_ROOT%" is not a WorldEngine checkout - CMakeLists.txt missing.
    exit /b 2
)

set "CMAKE_EXE="
for /f "delims=" %%P in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%P"
if not defined CMAKE_EXE call :find_vs_cmake
if not defined CMAKE_EXE (
    echo [error] cmake not found - neither on PATH nor in a Visual Studio install.
    exit /b 3
)

if not defined WE_GENERATOR set "WE_GENERATOR=Visual Studio 18 2026"
set "WE_BUILD_DIR=%~dp0build\x64-%WE_CONFIG%"

echo [info] engine root : %WE_ROOT%
echo [info] build dir   : %WE_BUILD_DIR%
echo [info] config      : %WE_CONFIG%
echo [info] cmake       : %CMAKE_EXE%

"%CMAKE_EXE%" -S "%~dp0." -B "%WE_BUILD_DIR%" -G "%WE_GENERATOR%" -A x64 -DCMAKE_BUILD_TYPE=%WE_CONFIG% -DWE_ROOT="%WE_ROOT%"
if errorlevel 1 (
    echo [error] cmake configure failed.
    exit /b 1
)

"%CMAKE_EXE%" --build "%WE_BUILD_DIR%" --config %WE_CONFIG% --target Game --parallel
if errorlevel 1 (
    echo [error] build failed.
    exit /b 1
)

echo [ok] %WE_BUILD_DIR%\bin\%WE_CONFIG%\Game\%WE_CONFIG%\Game.dll
exit /b 0

:read_launcher_root
set "WE_ROOT_RAW="
for %%F in ("%~dp0*-Edit.cmd" "%~dp0*-Play.cmd") do (
    if exist "%%~fF" if not defined WE_ROOT_RAW for /f "tokens=2 delims==" %%R in ('findstr /i /c:"WE_ROOT=" "%%~fF"') do set "WE_ROOT_RAW=%%R"
)
if not defined WE_ROOT_RAW exit /b 0
rem WE_ROOT_RAW looks like: E:\WorldEngine  plus the trailing quote of the cmd line.
set "WE_ROOT=%WE_ROOT_RAW:"=%"
if not exist "%WE_ROOT%\CMakeLists.txt" set "WE_ROOT="
exit /b 0

:guess_relative_root
for %%P in ("%~dp0..\..") do (
    if exist "%%~fP\CMakeLists.txt" if exist "%%~fP\Game\CMakeLists.txt" set "WE_ROOT=%%~fP"
)
exit /b 0

:find_vs_cmake
set "WE_VS_ROOT=%ProgramFiles%\Microsoft Visual Studio"
for %%V in (18 2025 2022) do (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if not defined CMAKE_EXE if exist "%WE_VS_ROOT%\%%V\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "CMAKE_EXE=%WE_VS_ROOT%\%%V\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        )
    )
)
exit /b 0
