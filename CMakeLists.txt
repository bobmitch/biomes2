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
  # Every submodule the engine links. Missing any one skips the engine, for
  # the reason above: the gate must never depend on them.
  set(_missing "")
  foreach(_sub JoltPhysics/Build lua RmlUi freetype)
    if(NOT EXISTS ${PROJECT_SOURCE_DIR}/third_party/${_sub}/CMakeLists.txt AND
       NOT EXISTS ${PROJECT_SOURCE_DIR}/third_party/${_sub}/lua.h)
      list(APPEND _missing ${_sub})
    endif()
  endforeach()
  if(_missing STREQUAL "")
    set(BIOMES2_ENGINE ON)
  else()
    message(STATUS
      "engine/: SKIPPED -- submodules not checked out: ${_missing}. "
      "Run  git submodule update --init --recursive  to build it. "
      "worldgen and validate, which are the determinism gate, do not need them.")
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

  # --- The UI and scripting layer: Lua 5.4 driving RmlUi (engine/ui/) ---
  #
  # All three are built from submodules, statically, and only here: nothing
  # in worldgen/ or validate links them, so the gate's build is unchanged.
  # FreeType is C, the one C dependency, so C is enabled for the engine only.
  enable_language(C)

  # FreeType: RmlUi's font engine. Every optional codec off -- the UI loads
  # plain TrueType and nothing else, and each of these would be one more
  # thing to find on a build machine.
  set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
  set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
  add_subdirectory(third_party/freetype EXCLUDE_FROM_ALL)
  add_library(Freetype::Freetype ALIAS freetype)

  # Lua, compiled as C++ so a script error unwinds with an exception rather
  # than a longjmp straight over the destructors of the C++ that called it.
  # lua.c and onelua.c are the standalone interpreter, ltests.c the test
  # harness: not the library.
  file(GLOB _lua_src ${PROJECT_SOURCE_DIR}/third_party/lua/l*.c)
  list(FILTER _lua_src EXCLUDE REGEX "/(lua|ltests|onelua)\\.c$")
  set_source_files_properties(${_lua_src} PROPERTIES LANGUAGE CXX)
  add_library(biomes2_lua STATIC ${_lua_src})
  target_include_directories(biomes2_lua PUBLIC ${PROJECT_SOURCE_DIR}/third_party/lua)
  if(MSVC)
    target_compile_options(biomes2_lua PRIVATE /W0 /TP /D_CRT_SECURE_NO_WARNINGS)
  endif()
  # RmlUi aliases Lua::Lua, and an alias cannot be aliased: an imported
  # interface target over the real library.
  add_library(Lua::Lua INTERFACE IMPORTED GLOBAL)
  set_target_properties(Lua::Lua PROPERTIES
    INTERFACE_LINK_LIBRARIES biomes2_lua
    INTERFACE_INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/third_party/lua)

  set(RMLUI_LUA_BINDINGS ON CACHE BOOL "" FORCE)
  set(RMLUI_LUA_BINDINGS_LIBRARY "lua_as_cxx" CACHE STRING "" FORCE)
  set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/RmlUi EXCLUDE_FROM_ALL)

  add_executable(engine WIN32
    engine/main.cpp
    engine/render/renderer.cpp
    engine/meshing/chunk_mesher.cpp
    engine/physics/physics_world.cpp
    engine/svdag/brick_store.cpp
    engine/svdag/brick_pager.cpp
    engine/field/world_gpu.cpp
    engine/trees/tree_mesh.cpp
    engine/trees/tree_field.cpp
    engine/trees/tree_renderer.cpp
    engine/trees/felling.cpp
    engine/foliage/foliage_renderer.cpp
    engine/render/sky.cpp
    engine/render/fog.cpp
    engine/render/weather.cpp
    engine/render/ocean_detail.cpp
    engine/render/clouds.cpp
    engine/render/rain.cpp
    engine/render/lightning.cpp
    engine/render/canopy.cpp
    engine/render/height_gi.cpp
    engine/render/near_field.cpp
    engine/render/sky_access.cpp
    engine/render/far_shadow.cpp
    engine/render/scan_library.cpp
    engine/render/forest_map.cpp
    engine/render/foam_pattern.cpp
    engine/rocks/rock_mesh.cpp
    engine/rocks/rock_field.cpp
    engine/rocks/rock_renderer.cpp
    engine/props/glow_mushrooms.cpp
    engine/props/forage.cpp
    engine/player/items.cpp
    engine/player/inventory.cpp
    engine/player/survival.cpp
    engine/player/icons.cpp
    engine/character/character_model.cpp
    engine/character/character_renderer.cpp
    engine/ui/ui.cpp
    engine/ui/ui_render_d3d12.cpp
    engine/ui/script_api.cpp
    third_party/RmlUi/Backends/RmlUi_Platform_Win32.cpp)
  target_include_directories(engine PRIVATE ${PROJECT_SOURCE_DIR}
                             ${PROJECT_SOURCE_DIR}/third_party/RmlUi/Backends)
  target_link_libraries(engine PRIVATE RmlUi::RmlUi imm32)
  # M1: the engine links the generator now. Track B was built against the
  # trivial field precisely so a pipeline bug could never be confused with a
  # worldgen bug; that separation ends here, which is what the milestone is.
  target_link_libraries(engine PRIVATE d3d12 dxgi d3dcompiler Jolt worldgen)
  if(MSVC)
    target_compile_options(engine PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS)
  endif()

  # DXC for Shader Model 6 (ADR-004 L0): the Windows SDK's dxcompiler.dll
  # and dxil.dll, copied beside engine.exe after every build. dxil.dll signs
  # the DXIL; D3D12 refuses an unsigned shader, so the two go together. The
  # newest SDK that has both wins. Missing is a WARNING, not an error -- the
  # SM 5.1 engine runs without them and only SM 6 shaders fail, by name, as a
  # missing Jolt only skips the engine (CLAUDE.md on the CI outage).
  set(BIOMES2_DXC_DIR "")
  if(DEFINED ENV{WindowsSdkBinPath} AND DEFINED ENV{WindowsSdkVersion})
    string(REGEX REPLACE "[\\\\/]$" "" _sdkver "$ENV{WindowsSdkVersion}")
    set(_cand "$ENV{WindowsSdkBinPath}/${_sdkver}/x64")
    if(EXISTS "${_cand}/dxcompiler.dll" AND EXISTS "${_cand}/dxil.dll")
      set(BIOMES2_DXC_DIR "${_cand}")
    endif()
  endif()
  if(NOT BIOMES2_DXC_DIR)
    file(GLOB _dxcs "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll")
    list(SORT _dxcs)
    list(REVERSE _dxcs)
    foreach(_d ${_dxcs})
      get_filename_component(_dir "${_d}" DIRECTORY)
      if(EXISTS "${_dir}/dxil.dll")
        set(BIOMES2_DXC_DIR "${_dir}")
        break()
      endif()
    endforeach()
  endif()
  if(BIOMES2_DXC_DIR)
    message(STATUS "engine: DXC from ${BIOMES2_DXC_DIR}")
    add_custom_command(TARGET engine POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${BIOMES2_DXC_DIR}/dxcompiler.dll" "${BIOMES2_DXC_DIR}/dxil.dll"
              "$<TARGET_FILE_DIR:engine>")
  else()
    message(WARNING "engine: no dxcompiler.dll + dxil.dll in any Windows SDK; "
                    "Shader Model 6 shaders (ray tracing) will not compile.")
  endif()
endif()
