@echo off
REM Builds the layer-3 quota spike. Run from the repository root.
REM /fp:precise is deliberate: never compile the field function with fast-math,
REM FMA contraction breaks CPU/GPU parity (CLAUDE.md invariant 8).

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64 failed & exit /b 1)

if not exist build mkdir build

REM _CRT_SECURE_NO_WARNINGS lives here, not in the portable header: worldgen/
REM must stay host-neutral and knows nothing about MSVC.
cl /nologo /O2 /std:c++17 /EHsc /fp:precise /W3 /MT /D_CRT_SECURE_NO_WARNINGS ^
   tools\spike\quota_spike.cpp ^
   worldgen\assign\assign.cpp ^
   worldgen\assign\archetypes.cpp ^
   /Fe:build\quota_spike.exe /Fo:build\ /link /INCREMENTAL:NO
exit /b %errorlevel%
