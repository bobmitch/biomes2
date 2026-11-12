cmake_minimum_required(VERSION 3.20)
project(biomes2 CXX)

# One build definition for local and CI. The golden hashes are sensitive to
# floating-point flags, so a second set of build rules would eventually pin a
# different world than the one CI checks.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Binaries land in build/ so the paths in the docs hold on every generator.
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} ${PROJECT_SOURCE_DIR}/build)
endforeach()

# Never fast-math. FMA contraction breaks CPU/GPU parity, and this build is
# the determinism authority for the golden hashes (ADR-001) -- a GPU cannot
# produce a stable hash at all.
function(worldgen_strict_fp target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /fp:precise /W3 /D_CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -ffp-contract=off -fno-fast-math -Wall)
  endif()
endfunction()

find_package(Threads REQUIRED)

# The generator: the field function, the cell scatter, world build, layer 3.
# Everything upstream of the golden hash lives here and nothing else does.
add_library(worldgen STATIC
  worldgen/assign/assign.cpp
  worldgen/assign/archetypes.cpp
  worldgen/world.cpp)
target_include_directories(worldgen PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(worldgen PUBLIC Threads::Threads)
worldgen_strict_fp(worldgen)

# The CI gate.
add_executable(validate tools/validate/validate.cpp)
target_link_libraries(validate PRIVATE worldgen)
worldgen_strict_fp(validate)

# The native viewer (ADR-002). Windows-only by design: it is a debug tool, it
# links the shipping generator directly, and CI is headless so determinism
# coverage does not depend on it.
if(WIN32 AND EXISTS ${PROJECT_SOURCE_DIR}/tools/view/view.cpp)
  add_executable(view WIN32 tools/view/view.cpp)
  target_link_libraries(view PRIVATE worldgen comctl32)
  worldgen_strict_fp(view)
endif()

# Track B: the engine (ADR-003 -- Direct3D 12). Windows-only, like the viewer.
# It does NOT link worldgen: Track B is built against the trivial analytic
# field in engine/field/ precisely so that pipeline bugs are never confused
# with worldgen bugs. M1 is where that changes.
#
# No strict-fp here on purpose. These flags exist to protect the golden
# hashes, nothing in engine/ feeds them, and pinning the renderer's maths
# would imply a determinism guarantee this binary does not make.
if(WIN32 AND EXISTS ${PROJECT_SOURCE_DIR}/engine/main.cpp)
  if(NOT EXISTS ${PROJECT_SOURCE_DIR}/third_party/JoltPhysics/Build/CMakeLists.txt)
    message(FATAL_ERROR
      "engine/ needs Jolt: run  git submodule update --init --recursive
"
      "(validate.exe, the CI gate, does not -- it never links the engine.)")
  endif()

  # OVERRIDE_CXX_FLAGS is OFF deliberately. Jolt defaults it ON, which rewrites
  # CMAKE_CXX_FLAGS_DEBUG and CMAKE_CXX_FLAGS_RELEASE. Those are the flags
  # invariant 8 rests on, and a dependency quietly editing the global C++ flags
  # is exactly how a project loses its determinism guarantee without noticing.
  # The golden hashes are what proves it did not: validate.exe must still pass.
  set(OVERRIDE_CXX_FLAGS OFF)
  set(DOUBLE_PRECISION OFF)
  # Jolt's own cross-platform determinism is not needed: physics never reaches
  # the golden hash, and it costs performance.
  set(CROSS_PLATFORM_DETERMINISTIC OFF)
  set(INTERPROCEDURAL_OPTIMIZATION OFF)
  set(ENABLE_ALL_WARNINGS OFF)
  set(TARGET_UNIT_TESTS OFF)
  set(TARGET_HELLO_WORLD OFF)
  set(TARGET_PERFORMANCE_TEST OFF)
  set(TARGET_SAMPLES OFF)
  set(TARGET_VIEWER OFF)
  add_subdirectory(third_party/JoltPhysics/Build EXCLUDE_FROM_ALL)

  # Jolt builds against the STATIC MSVC runtime by default; everything else
  # here uses the dynamic one, and mixing them is a link error. Jolt moves,
  # not the rest: worldgen and validate are the determinism authority, and
  # changing how they are built to suit a physics dependency is the wrong way
  # round. Needs CMP0091, which cmake_minimum_required(3.20) already sets NEW.
  set_property(TARGET Jolt PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

  add_executable(engine WIN32
    engine/main.cpp
    engine/render/renderer.cpp
    engine/meshing/chunk_mesher.cpp
    engine/physics/physics_world.cpp)
  target_include_directories(engine PRIVATE ${PROJECT_SOURCE_DIR})
  target_link_libraries(engine PRIVATE d3d12 dxgi d3dcompiler Jolt)
  if(MSVC)
    target_compile_options(engine PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS)
  endif()
endif()
