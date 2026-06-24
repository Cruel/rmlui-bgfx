set(RMLUI_BGFX_SHADER_PROGRAMS
    rmlui                  vs_rmlui.sc           fs_rmlui.sc
    rmlui_composite        vs_rmlui_composite.sc fs_rmlui_composite.sc
    rmlui_composite_filter vs_rmlui_composite.sc fs_rmlui_composite_filter.sc
    rmlui_copy             vs_rmlui_composite.sc fs_rmlui_composite.sc
    rmlui_opacity          vs_rmlui_composite.sc fs_rmlui_opacity.sc
    rmlui_color_matrix     vs_rmlui_composite.sc fs_rmlui_color_matrix.sc
    rmlui_mask_multiply    vs_rmlui_composite.sc fs_rmlui_mask_multiply.sc
    rmlui_blur             vs_rmlui_blur.sc      fs_rmlui_blur.sc
    rmlui_drop_shadow      vs_rmlui_composite.sc fs_rmlui_drop_shadow.sc
    rmlui_gradient         vs_rmlui.sc           fs_rmlui_gradient.sc
)

function(rmlui_bgfx_default_shader_variants out_var)
    if(EMSCRIPTEN)
        set(_variants essl-100)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
        set(_variants essl-300)
    else()
        set(_variants glsl-120)
    endif()
    set(${out_var} ${_variants} PARENT_SCOPE)
endfunction()

function(rmlui_bgfx_get_shader_variant)
    set(options)
    set(oneValueArgs VARIANT OUT_PLATFORM OUT_PROFILE)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "" ${ARGN})
    if(ARG_VARIANT STREQUAL "glsl-120")
        set(_platform linux)
        set(_profile 120)
    elseif(ARG_VARIANT STREQUAL "essl-100")
        set(_platform asm.js)
        set(_profile 100_es)
    elseif(ARG_VARIANT STREQUAL "essl-300")
        set(_platform android)
        set(_profile 300_es)
    else()
        message(FATAL_ERROR "Unknown rmlui-bgfx shader variant '${ARG_VARIANT}'")
    endif()
    set(${ARG_OUT_PLATFORM} "${_platform}" PARENT_SCOPE)
    set(${ARG_OUT_PROFILE} "${_profile}" PARENT_SCOPE)
endfunction()

function(rmlui_bgfx_collect_shader_outputs)
    set(options)
    set(oneValueArgs OUTPUT_ROOT OUT_VAR)
    set(multiValueArgs VARIANTS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    set(_outputs)
    foreach(_variant IN LISTS ARG_VARIANTS)
        rmlui_bgfx_get_shader_variant(VARIANT "${_variant}" OUT_PLATFORM _platform OUT_PROFILE _profile)
        list(LENGTH RMLUI_BGFX_SHADER_PROGRAMS _count)
        math(EXPR _last "${_count} - 1")
        foreach(_idx RANGE 0 ${_last} 3)
            list(GET RMLUI_BGFX_SHADER_PROGRAMS ${_idx} _program)
            list(APPEND _outputs
                "${ARG_OUTPUT_ROOT}/shaders/bgfx/${_variant}/${_program}.vs.bin"
                "${ARG_OUTPUT_ROOT}/shaders/bgfx/${_variant}/${_program}.fs.bin")
        endforeach()
    endforeach()
    set(${ARG_OUT_VAR} ${_outputs} PARENT_SCOPE)
endfunction()

function(rmlui_bgfx_collect_shader_inputs source_dir out_var)
    set(_inputs "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    list(APPEND _inputs "${source_dir}/varying.def.sc")
    list(LENGTH RMLUI_BGFX_SHADER_PROGRAMS _count)
    math(EXPR _last "${_count} - 1")
    foreach(_idx RANGE 0 ${_last} 3)
        math(EXPR _vs_idx "${_idx} + 1")
        math(EXPR _fs_idx "${_idx} + 2")
        list(GET RMLUI_BGFX_SHADER_PROGRAMS ${_vs_idx} _vs)
        list(GET RMLUI_BGFX_SHADER_PROGRAMS ${_fs_idx} _fs)
        list(APPEND _inputs "${source_dir}/${_vs}" "${source_dir}/${_fs}")
    endforeach()
    list(REMOVE_DUPLICATES _inputs)
    set(${out_var} ${_inputs} PARENT_SCOPE)
endfunction()

function(rmlui_bgfx_find_shader_tools)
    set(options REQUIRED)
    set(oneValueArgs OUT_SHADERC OUT_BGFX_INCLUDE)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "" ${ARGN})

    set(_shaderc "${RMLUI_BGFX_SHADERC_EXECUTABLE}")
    if(NOT _shaderc AND DEFINED ENV{SHADERC})
        set(_shaderc "$ENV{SHADERC}")
    endif()
    if(NOT _shaderc)
        find_program(_shaderc_found shaderc)
        set(_shaderc "${_shaderc_found}")
    endif()
    if(ARG_REQUIRED AND (NOT _shaderc OR NOT EXISTS "${_shaderc}"))
        message(FATAL_ERROR "shaderc host executable not found. Set RMLUI_BGFX_SHADERC_EXECUTABLE or SHADERC, or install bgfx[tools].")
    endif()

    set(_bgfx_include "${RMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR}")
    if(NOT _bgfx_include)
        foreach(_root IN LISTS CMAKE_PREFIX_PATH)
            file(GLOB _triplets LIST_DIRECTORIES true "${_root}/*")
            foreach(_triplet IN LISTS _triplets)
                if(EXISTS "${_triplet}/include/bgfx/bgfx_shader.sh")
                    set(_bgfx_include "${_triplet}/include/bgfx")
                    break()
                endif()
            endforeach()
        endforeach()
    endif()
    if(ARG_REQUIRED AND (NOT _bgfx_include OR NOT EXISTS "${_bgfx_include}/bgfx_shader.sh"))
        message(FATAL_ERROR "bgfx_shader.sh not found. Set RMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR to the bgfx shader include directory.")
    endif()

    set(${ARG_OUT_SHADERC} "${_shaderc}" PARENT_SCOPE)
    set(${ARG_OUT_BGFX_INCLUDE} "${_bgfx_include}" PARENT_SCOPE)
endfunction()

function(rmlui_bgfx_add_shader_target)
    set(options)
    set(oneValueArgs TARGET SHADERC SOURCE_DIR OUTPUT_ROOT BGFX_INCLUDE_DIR)
    set(multiValueArgs VARIANTS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    rmlui_bgfx_collect_shader_outputs(VARIANTS ${ARG_VARIANTS} OUTPUT_ROOT "${ARG_OUTPUT_ROOT}" OUT_VAR _outputs)
    rmlui_bgfx_collect_shader_inputs("${ARG_SOURCE_DIR}" _inputs)
    add_custom_command(
        OUTPUT ${_outputs}
        COMMAND "${CMAKE_COMMAND}"
            "-DRMLUI_BGFX_SHADERC_EXECUTABLE=${ARG_SHADERC}"
            "-DRMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR=${ARG_BGFX_INCLUDE_DIR}"
            "-DRMLUI_BGFX_SHADER_SOURCE_DIR=${ARG_SOURCE_DIR}"
            "-DRMLUI_BGFX_SHADER_OUTPUT_ROOT=${ARG_OUTPUT_ROOT}"
            "-DRMLUI_BGFX_SHADER_VARIANTS=$<JOIN:${ARG_VARIANTS},;>"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CompileRmlUiBgfxShaders.cmake"
        DEPENDS ${_inputs}
        COMMENT "Compiling rmlui-bgfx bgfx shaders"
        VERBATIM
        COMMAND_EXPAND_LISTS
    )
    add_custom_target(${ARG_TARGET} DEPENDS ${_outputs})
endfunction()
