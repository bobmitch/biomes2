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

# M2: the golden gate runs as part of the BUILD, not as something to remember.
#
# The discipline it enforces -- "the hashes moved, so revert or bump the
# version" -- only survives a year of tuning if forgetting is impossible, and
# a check you have to run by hand is a check you will skip on the commit that
# needed it. So: eight seeds, no sweep, no contact sheet, and a non-zero exit
# fails `cmake --build`.
#
# It depends on validate rather than on a list of sources, and that is the
# point: ANY change to a worldgen header or source relinks validate, so the
# gate re-runs. A list would be a second place to remember, which is the thing
# being designed out. The two data files are named explicitly because nothing
# compiles them.
set(GOLDEN_STAMP ${CMAKE_CURRENT_BINARY_DIR}/golden.stamp)
add_custom_command(
  OUTPUT ${GOLDEN_STAMP}
  COMMAND $<TARGET_FILE:validate> --golden
  COMMAND ${CMAKE_COMMAND} -E touch ${GOLDEN_STAMP}
  DEPENDS validate
          ${PROJECT_SOURCE_DIR}/worldgen/data/archetypes.json
          ${PROJECT_SOURCE_DIR}/worldgen/golden/golden.json
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  COMMENT "golden seeds: is this the world that is pinned?"
  VERBATIM)
add_custom_target(golden_gate ALL DEPENDS ${GOLDEN_STAMP})

# The native viewer (ADR-002). Windows-only by design: it is a debug tool, it
# links the shipping generator directly, and CI is headless so determinism
# coverage does not depend on it.
if(WIN32 AND EXISTS ${PROJECT_SOURCE_DIR}/tools/view/view.cpp)
  add_executable(view WIN32 tools/view/view.cpp)
  target_link_libraries(view PRIVATE worldgen comctl32)
  worldgen_strict_fp(view)
endif()

# Track B: the engine (ADR-003 -- Direct3D 12). Windows-only, like the viewer.
# Until M1 it did not link worldgen at all: Track B was built against the
# trivial analytic field in engine/field/ precisely so that pipeline bugs
# could never be confused with worldgen bugs. M1 ended that separation, which
# is what the milestone was.
#
# No strict-fp here on purpose. These flags exist to protect the golden
# hashes, nothing in engine/ feeds them, and pinning the renderer's maths
# would imply a determinism guarantee this binary does not make.
#
# A missing Jolt submodule SKIPS the engine. It used to be a FATAL_ERROR, and
# that error cost nine commits of Windows CI: actions/checkout does not fetch
# submodules by default, so every configure on windows-latest died here from
# the commit that added Jolt onwards. The Linux job stayed green and the run
# stayed red, which is the worst shape a failure can take -- the two-OS matrix
# is the ONLY thing that catches the libm divergence invariant 9 is about, and
# half of it was gone.
#
# The comment that error carried was already right: validate.exe never links
# the engine. So a checkout without the submodule is a perfectly good
# determinism gate, and refusing to configure one was the bug.
set(BIOMES2_ENGINE OFF)
if(WIN32 AND EXISTS ${PROJECT_SOURCE_DIR}/engine/main.cpp)
  if(EXISTS ${PROJECT_SOURCE_DIR}/third_party/JoltPhysics/Build/CMakeLists.txt)
    set(BIOMES2_ENGINE ON)
  else()
    message(STATUS
      "engine/: SKIPPED -- the Jolt submodule is not checked out. "
      "Run  git submodule update --init --recursive  to build it. "
      "worldgen and validate, which are the determinism gate, do not need it.")
  endif()
endif()

if(BIOMES2_ENGINE)

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
    engine/physics/physics_world.cpp
    engine/svdag/brick_store.cpp
    engine/svdag/brick_pager.cpp
    engine/field/world_gpu.cpp)
  target_include_directories(engine PRIVATE ${PROJECT_SOURCE_DIR})
  # M1: the engine links the generator now. Track B was built against the
  # trivial field precisely so a pipeline bug could never be confused with a
  # worldgen bug; that separation ends here, which is what the milestone is.
  target_link_libraries(engine PRIVATE d3d12 dxgi d3dcompiler Jolt worldgen)
  if(MSVC)
    target_compile_options(engine PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS)
  endif()
endif()
