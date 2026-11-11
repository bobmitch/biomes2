@echo off
REM Builds the validation harness and the native viewer. Run from the
REM repository root.
REM
REM A thin wrapper over CMakeLists.txt rather than its own compiler invocation:
REM the golden hashes are sensitive to floating-point flags, so a second set of
REM build rules would eventually pin a different world than CI checks.

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64 failed & exit /b 1)

cmake -S . -B build\cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

cmake --build build\cmake
exit /b %errorlevel%
