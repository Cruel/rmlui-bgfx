include("${CMAKE_CURRENT_LIST_DIR}/RmlUiBgfxShaders.cmake")

if(NOT RMLUI_BGFX_SHADERC_EXECUTABLE OR NOT EXISTS "${RMLUI_BGFX_SHADERC_EXECUTABLE}")
    message(FATAL_ERROR "RMLUI_BGFX_SHADERC_EXECUTABLE is not an executable host shaderc: ${RMLUI_BGFX_SHADERC_EXECUTABLE}")
endif()
if(NOT RMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR OR NOT EXISTS "${RMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR}/bgfx_shader.sh")
    message(FATAL_ERROR "RMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR must contain bgfx_shader.sh: ${RMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR}")
endif()

foreach(_variant IN LISTS RMLUI_BGFX_SHADER_VARIANTS)
    rmlui_bgfx_get_shader_variant(VARIANT "${_variant}" OUT_PLATFORM _platform OUT_PROFILE _profile)
    set(_out_dir "${RMLUI_BGFX_SHADER_OUTPUT_ROOT}/shaders/bgfx/${_variant}")
    file(MAKE_DIRECTORY "${_out_dir}")

    list(LENGTH RMLUI_BGFX_SHADER_PROGRAMS _count)
    math(EXPR _last "${_count} - 1")
    foreach(_idx RANGE 0 ${_last} 3)
        math(EXPR _vs_idx "${_idx} + 1")
        math(EXPR _fs_idx "${_idx} + 2")
        list(GET RMLUI_BGFX_SHADER_PROGRAMS ${_idx} _program)
        list(GET RMLUI_BGFX_SHADER_PROGRAMS ${_vs_idx} _vs)
        list(GET RMLUI_BGFX_SHADER_PROGRAMS ${_fs_idx} _fs)
        set(_vs_out "${_out_dir}/${_program}.vs.bin")
        set(_fs_out "${_out_dir}/${_program}.fs.bin")

        execute_process(
            COMMAND "${RMLUI_BGFX_SHADERC_EXECUTABLE}"
                -f "${RMLUI_BGFX_SHADER_SOURCE_DIR}/${_vs}"
                -o "${_vs_out}"
                --type vertex
                --platform "${_platform}"
                -p "${_profile}"
                -i "${RMLUI_BGFX_SHADER_SOURCE_DIR}"
                -i "${RMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR}"
            RESULT_VARIABLE _vs_result
        )
        if(NOT _vs_result EQUAL 0)
            message(FATAL_ERROR "shaderc failed for ${_vs} (${_variant})")
        endif()

        execute_process(
            COMMAND "${RMLUI_BGFX_SHADERC_EXECUTABLE}"
                -f "${RMLUI_BGFX_SHADER_SOURCE_DIR}/${_fs}"
                -o "${_fs_out}"
                --type fragment
                --platform "${_platform}"
                -p "${_profile}"
                -i "${RMLUI_BGFX_SHADER_SOURCE_DIR}"
                -i "${RMLUI_BGFX_BGFX_SHADER_INCLUDE_DIR}"
            RESULT_VARIABLE _fs_result
        )
        if(NOT _fs_result EQUAL 0)
            message(FATAL_ERROR "shaderc failed for ${_fs} (${_variant})")
        endif()
    endforeach()
endforeach()
