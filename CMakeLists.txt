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
