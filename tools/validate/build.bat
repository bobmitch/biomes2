@echo off
REM Builds the validation harness. Run from the repository root.
REM /fp:precise is deliberate: never compile the field function with fast-math,
REM FMA contraction breaks CPU/GPU parity (CLAUDE.md invariant 8).

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64 failed & exit /b 1)

if not exist build mkdir build

REM _CRT_SECURE_NO_WARNINGS lives here, not in the portable header: worldgen/
REM must stay host-neutral and knows nothing about MSVC.
REM /fp:contract is off under /fp:precise, which is the point: this build is the
REM determinism authority for golden hashes (ADR-001).
cl /nologo /O2 /std:c++17 /EHsc /fp:precise /W3 /MT /D_CRT_SECURE_NO_WARNINGS ^
   tools\validate\validate.cpp ^
   worldgen\assign\assign.cpp ^
   worldgen\assign\archetypes.cpp ^
   /Fe:build\validate.exe /Fo:build\ /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1

cl /nologo /O2 /std:c++17 /EHsc /W3 /MT /D_CRT_SECURE_NO_WARNINGS ^
   tools\wgslgen\wgslgen.cpp ^
   /Fe:build\wgslgen.exe /Fo:build\ /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1

if not exist tools\mapview\src\generated mkdir tools\mapview\src\generated
build\wgslgen.exe worldgen\shaders\noise.h tools\mapview\src\generated\noise.wgsl
if errorlevel 1 exit /b 1
build\wgslgen.exe worldgen\shaders\field.h tools\mapview\src\generated\field.wgsl
exit /b %errorlevel%
