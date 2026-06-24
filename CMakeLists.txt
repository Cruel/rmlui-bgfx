cmake_minimum_required(VERSION 3.24)
project(rmlui_bgfx VERSION 0.1.0 LANGUAGES CXX)

option(RMLUI_BGFX_BUILD_TESTS "Build rmlui-bgfx unit tests" ${PROJECT_IS_TOP_LEVEL})
option(RMLUI_BGFX_BUILD_SAMPLES "Build rmlui-bgfx sample backend experiments" OFF)
option(RMLUI_BGFX_ENABLE_RENDER_PERF "Compile renderer performance counters and perf logging hooks" OFF)
option(RMLUI_BGFX_INSTALL "Install rmlui-bgfx CMake package" ${PROJECT_IS_TOP_LEVEL})
if(RMLUI_BGFX_BUILD_SAMPLES)
    set(RMLUI_BGFX_INSTALL OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RmlUiBgfxShaders)

if(NOT TARGET bgfx::bgfx)
    find_package(bgfx CONFIG REQUIRED)
endif()

if(RMLUI_BGFX_BUILD_SAMPLES)
    set(RMLUI_BGFX_RMLUI_SOURCE_DIR "" CACHE PATH "Path to an upstream RmlUi source checkout used to build samples")
    if(NOT RMLUI_BGFX_RMLUI_SOURCE_DIR OR NOT EXISTS "${RMLUI_BGFX_RMLUI_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "RMLUI_BGFX_BUILD_SAMPLES requires RMLUI_BGFX_RMLUI_SOURCE_DIR to point at an upstream RmlUi source checkout")
    endif()
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

if(NOT TARGET RmlUi::RmlUi AND NOT TARGET RmlUi::Core AND NOT RMLUI_BGFX_BUILD_SAMPLES)
    find_package(RmlUi CONFIG REQUIRED)
endif()

if(TARGET RmlUi::RmlUi)
    set(RMLUI_BGFX_RMLUI_TARGET RmlUi::RmlUi)
elseif(TARGET RmlUi::Core)
    set(RMLUI_BGFX_RMLUI_TARGET RmlUi::Core)
else()
    message(FATAL_ERROR "rmlui-bgfx requires RmlUi::RmlUi or RmlUi::Core")
endif()

add_library(rmlui_bgfx STATIC
    src/bounds.cpp
    src/draw.cpp
    src/filters.cpp
    src/layers.cpp
    src/pass_scheduler.cpp
    src/passes.cpp
    src/planning.cpp
    src/render_interface.cpp
    src/target_cache.cpp
)
add_library(rmlui_bgfx::rmlui_bgfx ALIAS rmlui_bgfx)

set_target_properties(rmlui_bgfx PROPERTIES
    EXPORT_NAME rmlui_bgfx
    POSITION_INDEPENDENT_CODE ON
)

target_compile_features(rmlui_bgfx PUBLIC cxx_std_20)
target_include_directories(rmlui_bgfx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/private
)
target_link_libraries(rmlui_bgfx PUBLIC bgfx::bgfx ${RMLUI_BGFX_RMLUI_TARGET})
if(TARGET bx::bx)
    target_link_libraries(rmlui_bgfx PUBLIC bx::bx)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(rmlui_bgfx PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
    )
endif()
if(RMLUI_BGFX_ENABLE_RENDER_PERF)
    target_compile_definitions(rmlui_bgfx PRIVATE RMLUI_BGFX_ENABLE_RENDER_PERF=1)
endif()

set(RMLUI_BGFX_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bgfx" CACHE INTERNAL "rmlui-bgfx shader source root")

target_compile_definitions(rmlui_bgfx PUBLIC
    RMLUI_BGFX_SHADER_SOURCE_DIR="$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/shaders/bgfx>"
)

if(RMLUI_BGFX_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        find_package(Catch2 3 CONFIG REQUIRED)
        include(Catch)
        add_executable(rmlui_bgfx_tests
            tests/rmlui_conservative_mask_bounds_tests.cpp
            tests/rmlui_layer_system_tests.cpp
            tests/rmlui_render_bounds_tests.cpp
            tests/rmlui_render_interface_lifecycle_tests.cpp
            tests/rmlui_render_pass_scheduler_tests.cpp
            tests/rmlui_render_planning_tests.cpp
        )
        target_include_directories(rmlui_bgfx_tests PRIVATE src/private)
        target_link_libraries(rmlui_bgfx_tests PRIVATE rmlui_bgfx::rmlui_bgfx Catch2::Catch2WithMain)
        catch_discover_tests(rmlui_bgfx_tests)
    endif()
endif()

if(RMLUI_BGFX_BUILD_SAMPLES)
    add_subdirectory("samples")
endif()

if(RMLUI_BGFX_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS rmlui_bgfx
        EXPORT rmlui_bgfxTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY shaders/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rmlui-bgfx/shaders)
    install(FILES cmake/RmlUiBgfxShaders.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rmlui_bgfx)

    configure_package_config_file(
        cmake/rmlui_bgfxConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/rmlui_bgfxConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rmlui_bgfx
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/rmlui_bgfxConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/rmlui_bgfxConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/rmlui_bgfxConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rmlui_bgfx
    )
    install(EXPORT rmlui_bgfxTargets
        NAMESPACE rmlui_bgfx::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rmlui_bgfx
    )
endif()
