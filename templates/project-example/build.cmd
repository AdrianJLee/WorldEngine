@echo off
rem ===========================================================================
rem WorldEngine project build (PROJ-8/T2; PROJ-12/T1 adds the fast path that
rem reuses an already built engine): builds this project's own Game.dll.
rem
rem Output: project\build\x64-CONFIG\bin\CONFIG\Game\CONFIG\Game.dll
rem         plus WorldRuntime.dll - the engine runtime - next to it (the fast path
rem         copies it from the engine build directory).
rem
rem Build modes:
rem   fast   - WE_ENGINE_BUILD_DIR is a prebuilt engine build directory
rem            (<engine>\build\x64-CONFIG) containing
rem              Engine\CONFIG\WorldRuntime.lib / WorldRuntime.dll
rem              Engine\generators\schema-compiler\CONFIG\schema-compiler.exe
rem            -> only this project's Game.dll is compiled; the engine is reused.
rem            Missing artifacts are a hard error (no silent fallback).
rem   source - the engine is added with add_subdirectory and compiled here.
rem
rem Engine root resolution order (same as the project CMakeLists.txt):
rem   1) first argument: build.cmd ENGINE_ROOT [CONFIG] [ENGINE_BUILD_DIR]
rem   2) environment variable WE_ROOT
rem   3) <project>\.we\engine-root.txt (one line, written by the build entry point)
rem   4) ..\.. - the template lives inside the engine checkout under templates\
rem   5) legacy: engine root recorded in a wizard-generated *-Edit.cmd / *-Play.cmd
rem
rem Engine build dir resolution order:
rem   a) third argument / environment variable WE_ENGINE_BUILD_DIR (must be complete)
rem   b) <engine root>\build\x64-CONFIG when it is complete (auto fast path)
rem   c) otherwise source mode
rem WE_FORCE_SOURCE_MODE=1 skips a/b and always builds the engine from source
rem (required while editing engine sources - the fast path reuses old binaries).
rem
rem Usage: build.cmd
rem        build.cmd E:\WorldEngine
rem        build.cmd E:\WorldEngine Release
rem        build.cmd E:\WorldEngine Debug E:\WorldEngine\build\x64-Debug
rem ===========================================================================
setlocal EnableExtensions
set "WE_CONFIG=%~2"
if "%WE_CONFIG%"=="" set "WE_CONFIG=Debug"

rem ---- engine root ----
if not "%~1"=="" (
    set "WE_ROOT=%~1"
    set "WE_ROOT_SOURCE=argument 1"
)
if defined WE_ROOT if not defined WE_ROOT_SOURCE set "WE_ROOT_SOURCE=environment variable WE_ROOT"
if not defined WE_ROOT call :read_engine_root_file
if not defined WE_ROOT call :guess_relative_root
if not defined WE_ROOT call :read_launcher_root

if not defined WE_ROOT (
    echo [error] WorldEngine engine root not found.
    echo         Usage: build.cmd ENGINE_ROOT [Debug or Release] [ENGINE_BUILD_DIR]
    echo         Or set WE_ROOT to the checkout that owns CMakeLists.txt, Engine and Game,
    echo         or write that path into .we\engine-root.txt next to this file.
    exit /b 2
)
if not exist "%WE_ROOT%\CMakeLists.txt" (
    echo [error] "%WE_ROOT%" is not a WorldEngine checkout - CMakeLists.txt missing.
    exit /b 2
)

rem ---- engine build dir (fast path) ----
if not "%~3"=="" (
    set "WE_ENGINE_BUILD_DIR=%~3"
    set "WE_ENGINE_BUILD_DIR_SOURCE=argument 3"
)
if defined WE_ENGINE_BUILD_DIR if not defined WE_ENGINE_BUILD_DIR_SOURCE set "WE_ENGINE_BUILD_DIR_SOURCE=environment variable WE_ENGINE_BUILD_DIR"

set "WE_FORCE_SOURCE=OFF"
if defined WE_FORCE_SOURCE_MODE if not "%WE_FORCE_SOURCE_MODE%"=="0" set "WE_FORCE_SOURCE=ON"

set "WE_MODE=source"
if "%WE_FORCE_SOURCE%"=="ON" goto :forced_source_mode
if defined WE_ENGINE_BUILD_DIR goto :check_engine_build_dir

set "WE_ENGINE_BUILD_DIR=%WE_ROOT%\build\x64-%WE_CONFIG%"
if not exist "%WE_ENGINE_BUILD_DIR%\Engine\%WE_CONFIG%\WorldRuntime.lib" goto :engine_build_dir_incomplete
if not exist "%WE_ENGINE_BUILD_DIR%\Engine\%WE_CONFIG%\WorldRuntime.dll" goto :engine_build_dir_incomplete
if not exist "%WE_ENGINE_BUILD_DIR%\Engine\generators\schema-compiler\%WE_CONFIG%\schema-compiler.exe" goto :engine_build_dir_incomplete
set "WE_ENGINE_BUILD_DIR_SOURCE=auto-detected %WE_ROOT%\build\x64-%WE_CONFIG%
set "WE_ENGINE_BUILD_DIR_IS_AUTO=1"
goto :check_engine_build_dir

:engine_build_dir_incomplete
set "WE_ENGINE_BUILD_DIR_PROBE=%WE_ENGINE_BUILD_DIR%"
set "WE_ENGINE_BUILD_DIR="
set "WE_ENGINE_BUILD_DIR_SOURCE="
set "WE_ENGINE_BUILD_DIR_IS_AUTO="
if not exist "%WE_ENGINE_BUILD_DIR_PROBE%" goto :engine_build_dir_ok
echo [info] "%WE_ENGINE_BUILD_DIR_PROBE%" exists but is missing the %WE_CONFIG% WorldRuntime.lib / .dll
echo [info] or schema-compiler.exe; using source mode. Build the engine there for the fast path.
goto :engine_build_dir_ok

:forced_source_mode
echo [info] WE_FORCE_SOURCE_MODE is set; the engine is built from source.
set "WE_ENGINE_BUILD_DIR="
set "WE_ENGINE_BUILD_DIR_SOURCE="
goto :engine_build_dir_ok

:check_engine_build_dir
set "WE_FAST_LIB=%WE_ENGINE_BUILD_DIR%\Engine\%WE_CONFIG%\WorldRuntime.lib"
set "WE_FAST_DLL=%WE_ENGINE_BUILD_DIR%\Engine\%WE_CONFIG%\WorldRuntime.dll"
set "WE_FAST_SCHEMA=%WE_ENGINE_BUILD_DIR%\Engine\generators\schema-compiler\%WE_CONFIG%\schema-compiler.exe"
if not exist "%WE_FAST_LIB%" goto :engine_build_dir_broken
if not exist "%WE_FAST_DLL%" goto :engine_build_dir_broken
if not exist "%WE_FAST_SCHEMA%" goto :engine_build_dir_broken
set "WE_MODE=fast"
goto :engine_build_dir_ok

:engine_build_dir_broken
if defined WE_ENGINE_BUILD_DIR_IS_AUTO goto :engine_build_dir_incomplete
echo [error] WE_ENGINE_BUILD_DIR is not a complete %WE_CONFIG% engine build:
echo         %WE_ENGINE_BUILD_DIR%
echo         expected:
echo           %WE_FAST_LIB%
echo           %WE_FAST_DLL%
echo           %WE_FAST_SCHEMA%
echo         The engine was probably not built for %WE_CONFIG%, or the path is wrong.
echo         Build the engine there with -DCMAKE_BUILD_TYPE=%WE_CONFIG% and --config %WE_CONFIG%,
echo         or unset WE_ENGINE_BUILD_DIR to build the engine from source.
exit /b 4

:engine_build_dir_ok
set "CMAKE_EXE="
for /f "delims=" %%P in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%P"
if not defined CMAKE_EXE call :find_vs_cmake
if not defined CMAKE_EXE (
    echo [error] cmake not found - neither on PATH nor in a Visual Studio install.
    exit /b 3
)

if not defined WE_GENERATOR set "WE_GENERATOR=Visual Studio 18 2026"
set "WE_BUILD_DIR=%~dp0build\x64-%WE_CONFIG%"

echo [info] engine root : %WE_ROOT%  (%WE_ROOT_SOURCE%)
echo [info] build mode  : %WE_MODE%
if "%WE_MODE%"=="fast" (
    echo [info] reuse engine: "%WE_ENGINE_BUILD_DIR%"  [%WE_ENGINE_BUILD_DIR_SOURCE%]
    echo [info]   lib       : %WE_FAST_LIB%
    echo [info]   dll       : %WE_FAST_DLL%
    echo [info]   compiler  : %WE_FAST_SCHEMA%
) else (
    echo [info] engine src  : %WE_ROOT%  - the engine is compiled here with this project
)
if defined WE_SDK_ROOT echo [info] WE_SDK_ROOT is set but the engine SDK mode is not implemented yet; ignoring it.
echo [info] build dir   : %WE_BUILD_DIR%
echo [info] config      : %WE_CONFIG%
echo [info] cmake       : %CMAKE_EXE%

if "%WE_MODE%"=="fast" (
    "%CMAKE_EXE%" -S "%~dp0." -B "%WE_BUILD_DIR%" -G "%WE_GENERATOR%" -A x64 -DCMAKE_BUILD_TYPE=%WE_CONFIG% -DWE_ROOT="%WE_ROOT%" -DWE_ENGINE_BUILD_DIR="%WE_ENGINE_BUILD_DIR%" -DWE_FORCE_SOURCE_MODE=OFF
) else (
    "%CMAKE_EXE%" -S "%~dp0." -B "%WE_BUILD_DIR%" -G "%WE_GENERATOR%" -A x64 -DCMAKE_BUILD_TYPE=%WE_CONFIG% -DWE_ROOT="%WE_ROOT%" -DWE_ENGINE_BUILD_DIR= -DWE_FORCE_SOURCE_MODE=%WE_FORCE_SOURCE%
)
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
echo [ok] %WE_BUILD_DIR%\bin\%WE_CONFIG%\Game\%WE_CONFIG%\WorldRuntime.dll
exit /b 0

:read_engine_root_file
if not exist "%~dp0.we\engine-root.txt" exit /b 0
set "WE_ROOT_RAW="
for /f "usebackq delims=" %%L in ("%~dp0.we\engine-root.txt") do if not defined WE_ROOT_RAW set "WE_ROOT_RAW=%%L"
if not defined WE_ROOT_RAW exit /b 0
set "WE_ROOT_RAW=%WE_ROOT_RAW:"=%"
if not exist "%WE_ROOT_RAW%\CMakeLists.txt" exit /b 0
set "WE_ROOT=%WE_ROOT_RAW%"
set "WE_ROOT_SOURCE=.we\engine-root.txt"
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
if defined WE_ROOT set "WE_ROOT_SOURCE=legacy *-Edit.cmd / *-Play.cmd"
exit /b 0

:guess_relative_root
for %%P in ("%~dp0..\..") do (
    if exist "%%~fP\CMakeLists.txt" if exist "%%~fP\Game\CMakeLists.txt" (
        set "WE_ROOT=%%~fP"
        set "WE_ROOT_SOURCE=relative layout ..\.."
    )
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
