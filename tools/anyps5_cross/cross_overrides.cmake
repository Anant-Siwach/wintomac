# Injected into AnyPS5's top-level project with -DCMAKE_PROJECT_Relinker_INCLUDE=<this file> by
# tools/anyps5_build_win.sh (cross-compiling AnyPS5's Windows libraries on macOS; docs/ps5.md).
#
# AnyPS5's build runs its own programs while building the libraries (nid_patcher, pe_import_first,
# glslang-standalone, and SQLite's lemon, mkkeywordhash, mksourceid and jimsh). In a cross build CMake compiles them for Windows, so they cannot run on the Mac. Rather than
# patching every `COMMAND $<TARGET_FILE:...>`, the link step of those targets is replaced (RULE_LAUNCH_LINK) by a copy
# of the native build of the same sources (tools/anyps5_cross/host_tools, ANYPS5_HOST_TOOLS_DIR): every rule that
# uses $<TARGET_FILE:nid_patcher> then runs the Mac program, with AnyPS5's dependency graph unchanged.
if(NOT ANYPS5_HOST_TOOLS_DIR)
    message(FATAL_ERROR "cross_overrides.cmake: set ANYPS5_HOST_TOOLS_DIR (native nid_patcher, pe_import_first, glslang)")
endif()
set(_anyps5_cross_dir "${CMAKE_CURRENT_LIST_DIR}")
function(_anyps5_cross_host_tools)
    foreach(pair IN ITEMS "nid_patcher=nid_patcher" "pe_import_first=pe_import_first"
                          "glslang-standalone=glslang/StandAlone/glslang"
                          "sqlite_lemon=sqlite/lemon" "sqlite_mkkeywordhash=sqlite/mkkeywordhash"
                          "sqlite_mksourceid=sqlite/mksourceid" "sqlite_jimsh=sqlite/jimsh")
        string(REPLACE "=" ";" pair "${pair}")
        list(GET pair 0 target)
        list(GET pair 1 host)
        if(NOT TARGET ${target})
            continue()
        endif()
        get_target_property(imported ${target} IMPORTED)
        if(imported)
            continue()
        endif()
        if(NOT EXISTS "${ANYPS5_HOST_TOOLS_DIR}/${host}")
            message(FATAL_ERROR "cross_overrides.cmake: missing native ${ANYPS5_HOST_TOOLS_DIR}/${host}")
        endif()
        set_property(TARGET ${target} PROPERTY RULE_LAUNCH_LINK
            "${_anyps5_cross_dir}/host_tool_link.sh ${ANYPS5_HOST_TOOLS_DIR}/${host}")
        message(STATUS "cross build: ${target} runs the native ${ANYPS5_HOST_TOOLS_DIR}/${host}")
    endforeach()
endfunction()
# Runs at the end of the top-level CMakeLists.txt, after every subdirectory defined its targets.
cmake_language(DEFER CALL _anyps5_cross_host_tools)
