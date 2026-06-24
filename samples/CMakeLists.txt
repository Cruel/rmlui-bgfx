set(RMLUI_BGFX_RMLUI_SOURCE_DIR "" CACHE PATH "Path to an upstream RmlUi source checkout used to build samples")
set(RMLUI_BGFX_SAMPLE_SHADER_VARIANTS "" CACHE STRING "Semicolon-separated bgfx shader variants to compile for samples")

if(NOT RMLUI_BGFX_RMLUI_SOURCE_DIR OR NOT EXISTS "${RMLUI_BGFX_RMLUI_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "RMLUI_BGFX_BUILD_SAMPLES requires RMLUI_BGFX_RMLUI_SOURCE_DIR to point at an upstream RmlUi source checkout")
endif()

get_filename_component(RMLUI_BGFX_RMLUI_SOURCE_DIR "${RMLUI_BGFX_RMLUI_SOURCE_DIR}" ABSOLUTE)

find_package(SDL3 CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)
find_package(Lua REQUIRED)
if(NOT TARGET RmlUi::External::Lua)
    add_library(rmlui_bgfx_external_lua INTERFACE)
    target_include_directories(rmlui_bgfx_external_lua INTERFACE ${LUA_INCLUDE_DIR})
    target_link_libraries(rmlui_bgfx_external_lua INTERFACE ${LUA_LIBRARIES})
    add_library(RmlUi::External::Lua ALIAS rmlui_bgfx_external_lua)
endif()

if(NOT TARGET rmlui_core)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
    set(RMLUI_TESTS OFF CACHE BOOL "" FORCE)
    set(RMLUI_LUA_BINDINGS ON CACHE BOOL "" FORCE)
    set(RMLUI_LUA_BINDINGS_LIBRARY "lua" CACHE STRING "" FORCE)
    set(RMLUI_LOTTIE_PLUGIN ON CACHE BOOL "" FORCE)
    set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
    set(RMLUI_HARFBUZZ_SAMPLE ON CACHE BOOL "" FORCE)
    set(RMLUI_BACKEND "auto" CACHE STRING "" FORCE)
    add_subdirectory("${RMLUI_BGFX_RMLUI_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/rmlui-source" EXCLUDE_FROM_ALL)
endif()

if(NOT RMLUI_BGFX_SAMPLE_SHADER_VARIANTS)
    rmlui_bgfx_default_shader_variants(_sample_variants)
else()
    set(_sample_variants ${RMLUI_BGFX_SAMPLE_SHADER_VARIANTS})
endif()
list(GET _sample_variants 0 _sample_runtime_variant)

rmlui_bgfx_find_shader_tools(OUT_SHADERC _shaderc OUT_BGFX_INCLUDE _bgfx_shader_include)
if(NOT _bgfx_shader_include AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    set(_candidate_bgfx_include "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/bgfx")
    if(EXISTS "${_candidate_bgfx_include}/bgfx_shader.sh")
        set(_bgfx_shader_include "${_candidate_bgfx_include}")
    endif()
endif()
if(NOT _shaderc OR NOT EXISTS "${_shaderc}")
    message(FATAL_ERROR "shaderc host executable not found. Set RMLUI_BGFX_SHADERC_EXECUTABLE or SHADERC, or install bgfx[tools].")
endif()
if(NOT _bgfx_shader_include OR NOT EXISTS "${_bgfx_shader_include}/bgfx_shader.sh")
    message(FATAL_ERROR "bgfx_shader.sh not found. Set RMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR to the bgfx shader include directory.")
endif()
set(_sample_shader_output_root "${CMAKE_CURRENT_BINARY_DIR}/runtime-assets")
rmlui_bgfx_add_shader_target(
    TARGET rmlui_bgfx_sample_shaders
    SHADERC "${_shaderc}"
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shaders/bgfx"
    OUTPUT_ROOT "${_sample_shader_output_root}"
    BGFX_INCLUDE_DIR "${_bgfx_shader_include}"
    VARIANTS ${_sample_variants}
)
set(_sample_shader_runtime_dir "${_sample_shader_output_root}/shaders/bgfx/${_sample_runtime_variant}")

add_library(rmlui_backend_SDL_BGFX INTERFACE)
target_sources(rmlui_backend_SDL_BGFX INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/RmlUi_Backend_SDL_BGFX.cpp"
    "${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Backends/RmlUi_Platform_SDL.cpp"
    "${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Backends/RmlUi_Backend.h"
    "${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Backends/RmlUi_Platform_SDL.h"
)
target_include_directories(rmlui_backend_SDL_BGFX INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Backends"
)
target_compile_definitions(rmlui_backend_SDL_BGFX INTERFACE
    RMLUI_SDL_VERSION_MAJOR=3
    RMLUI_BGFX_SAMPLE_SHADER_DIR="${_sample_shader_runtime_dir}"
)
target_link_libraries(rmlui_backend_SDL_BGFX INTERFACE
    rmlui_bgfx::rmlui_bgfx
    SDL3::SDL3
)

include("${RMLUI_BGFX_RMLUI_SOURCE_DIR}/CMake/DependenciesForShell.cmake")

set(_saved_project_source_dir "${PROJECT_SOURCE_DIR}")
set(PROJECT_SOURCE_DIR "${RMLUI_BGFX_RMLUI_SOURCE_DIR}")
set(RMLUI_BACKEND "SDL_BGFX")
set(RMLUI_FONT_ENGINE_ENABLED TRUE)
set(RMLUI_SAMPLE_PREFIX "rmlui_bgfx_sample_")
set(RMLUI_TUTORIAL_PREFIX "rmlui_bgfx_tutorial_")
set(RMLUI_SHELL ON)
set(RMLUI_SAMPLES ON)

add_subdirectory("${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Samples/shell" "${CMAKE_CURRENT_BINARY_DIR}/shell")
add_subdirectory("${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Samples/basic" "${CMAKE_CURRENT_BINARY_DIR}/basic")

if(RMLUI_FONT_ENGINE_ENABLED)
    add_subdirectory("${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Samples/invaders" "${CMAKE_CURRENT_BINARY_DIR}/invaders")
    add_subdirectory("${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Samples/tutorial" "${CMAKE_CURRENT_BINARY_DIR}/tutorial")
endif()
if(RMLUI_FONT_ENGINE_ENABLED AND RMLUI_LUA_BINDINGS)
    add_subdirectory("${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Samples/lua_invaders" "${CMAKE_CURRENT_BINARY_DIR}/lua_invaders")
endif()

if(RMLUI_FONT_ENGINE_ENABLED AND NOT TARGET rmlui_bgfx_sample_effects)
    add_executable(rmlui_bgfx_sample_effects WIN32
        "${RMLUI_BGFX_RMLUI_SOURCE_DIR}/Samples/basic/effects/src/main.cpp"
    )
    set_common_target_options(rmlui_bgfx_sample_effects)
    target_link_libraries(rmlui_bgfx_sample_effects PRIVATE rmlui_shell)
endif()

set(PROJECT_SOURCE_DIR "${_saved_project_source_dir}")

set(_rmlui_bgfx_sample_targets
    rmlui_bgfx_sample_animation
    rmlui_bgfx_sample_benchmark
    rmlui_bgfx_sample_bitmap_font
    rmlui_bgfx_sample_custom_log
    rmlui_bgfx_sample_data_binding
    rmlui_bgfx_sample_demo
    rmlui_bgfx_sample_drag
    rmlui_bgfx_sample_effects
    rmlui_bgfx_sample_harfbuzz
    rmlui_bgfx_sample_invaders
    rmlui_bgfx_sample_load_document
    rmlui_bgfx_sample_lottie
    rmlui_bgfx_sample_lua_invaders
    rmlui_bgfx_sample_svg
    rmlui_bgfx_sample_transform
    rmlui_bgfx_sample_tree_view
    rmlui_bgfx_sample_variables
    rmlui_tutorial_drag
    rmlui_tutorial_template
)

add_custom_target(samples-all)
add_dependencies(samples-all rmlui_bgfx_sample_shaders)
foreach(_target IN LISTS _rmlui_bgfx_sample_targets)
    if(TARGET ${_target})
        add_dependencies(${_target} rmlui_bgfx_sample_shaders)
        add_dependencies(samples-all ${_target})
    endif()
endforeach()
