@echo off
rem Builds newen (32-bit) with Visual Studio 2026's compiler, CMake and Ninja. Output: bin\newen.exe
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build\build.ninja (
	"%CMAKE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_MAKE_PROGRAM=%NINJA%" -DCMAKE_C_COMPILER=cl || exit /b 1
)
"%CMAKE%" --build build || exit /b 1
echo.
echo Built bin\newen.exe
