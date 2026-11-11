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

# Never fast-math. FMA contraction breaks CPU/GPU parity (CLAUDE.md invariant
# 8), and this build is the determinism authority for the golden hashes
# (ADR-001) -- a GPU cannot produce a stable hash at all.
function(worldgen_strict_fp target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /fp:precise /W3 /D_CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -ffp-contract=off -fno-fast-math -Wall)
  endif()
endfunction()

find_package(Threads REQUIRED)

# Layer 3. Portable C++, no host dependencies.
add_library(worldgen_assign STATIC
  worldgen/assign/assign.cpp
  worldgen/assign/archetypes.cpp)
worldgen_strict_fp(worldgen_assign)

# The CI gate.
add_executable(validate tools/validate/validate.cpp)
target_link_libraries(validate PRIVATE worldgen_assign Threads::Threads)
worldgen_strict_fp(validate)

# Emits WGSL from the field-function dialect. No strict-fp needed: it moves
# text around and evaluates nothing.
add_executable(wgslgen tools/wgslgen/wgslgen.cpp)
if(MSVC)
  target_compile_options(wgslgen PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS)
endif()

# Generating the WGSL is part of building, so a dialect change the generator
# cannot handle fails here rather than in a browser later.
#
# A real OUTPUT/DEPENDS rule rather than a POST_BUILD hook: POST_BUILD only
# fires when wgslgen itself relinks, so editing worldgen/shaders/ left the
# generated WGSL silently stale -- which is precisely the drift the generated
# -not-hand-written decision exists to prevent.
set(GEN_DIR ${PROJECT_SOURCE_DIR}/tools/mapview/src/generated)
set(DIALECT_SOURCES
  ${PROJECT_SOURCE_DIR}/worldgen/shaders/noise.h
  ${PROJECT_SOURCE_DIR}/worldgen/shaders/field.h)

add_custom_command(
  OUTPUT ${GEN_DIR}/noise.wgsl ${GEN_DIR}/field.wgsl
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN_DIR}
  COMMAND wgslgen ${PROJECT_SOURCE_DIR}/worldgen/shaders/noise.h ${GEN_DIR}/noise.wgsl
  COMMAND wgslgen ${PROJECT_SOURCE_DIR}/worldgen/shaders/field.h ${GEN_DIR}/field.wgsl
  DEPENDS wgslgen ${DIALECT_SOURCES}
  COMMENT "Generating WGSL from worldgen/shaders/")

add_custom_target(wgsl ALL DEPENDS ${GEN_DIR}/noise.wgsl ${GEN_DIR}/field.wgsl)
add_dependencies(validate wgsl)
