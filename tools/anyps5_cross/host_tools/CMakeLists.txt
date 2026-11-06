# Native (macOS arm64) builds of the programs AnyPS5's Windows library build runs at build time:
#   nid_patcher       (core/libs/nid: renames PE exports to PS5 NIDs; runs on every .prx)
#   pe_import_first   (core/libs/allocator: moves mimalloc-redirect.dll to the front of mimalloc.dll's imports)
#   glslang-standalone (3rdparty/glslang: compiles the AGC driver's GLSL helper shaders to SPIR-V)
#   sqlite_lemon, sqlite_mkkeywordhash, sqlite_mksourceid, sqlite_jimsh (cmake/Sqlite.cmake: generate SQLite's parser
#                     and headers)
# In a cross build these targets are compiled for Windows and cannot run on the build host; tools/anyps5_cross/
# cross_overrides.cmake replaces their link step with a copy of the programs built here.
#   cmake -S tools/anyps5_cross/host_tools -B <dir> -DANYPS5_SRC=<AnyPS5 checkout>
cmake_minimum_required(VERSION 3.20)
project(anyps5_host_tools CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT ANYPS5_SRC)
    message(FATAL_ERROR "set ANYPS5_SRC to the AnyPS5 source tree")
endif()
set(N ${ANYPS5_SRC}/core/libs/nid/src)
add_executable(nid_patcher ${N}/Sha1.cpp ${N}/NidCompute.cpp ${N}/NidResolver.cpp ${N}/ExportExclusions.cpp
    ${N}/ElfNidPatcher.cpp ${N}/PeNidPatcher.cpp ${N}/MachONidPatcher.cpp ${N}/BinaryPatcherFactory.cpp ${N}/main.cpp)
target_include_directories(nid_patcher PRIVATE ${ANYPS5_SRC}/core/libs/nid/include)
add_executable(pe_import_first ${ANYPS5_SRC}/core/libs/allocator/PeImportFirstMain.cpp
    ${ANYPS5_SRC}/core/libs/allocator/PeImportFirst.cpp)
target_include_directories(pe_import_first PRIVATE ${ANYPS5_SRC}/core/libs)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_EXTERNAL OFF)
set(GLSLANG_TESTS OFF)
set(GLSLANG_ENABLE_INSTALL OFF)
set(ENABLE_GLSLANG_BINARIES ON)
set(ENABLE_SPIRV ON)
set(ENABLE_SPVREMAPPER OFF)
set(ENABLE_OPT OFF)
set(ENABLE_HLSL OFF)
add_subdirectory(${ANYPS5_SRC}/3rdparty/glslang ${CMAKE_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
set(SQ ${ANYPS5_SRC}/3rdparty/sqlite)
foreach(tool IN ITEMS lemon mkkeywordhash mksourceid)
    add_executable(sqlite_${tool} ${SQ}/tool/${tool}.c)
    set_target_properties(sqlite_${tool} PROPERTIES OUTPUT_NAME ${tool} RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/sqlite)
endforeach()
add_executable(sqlite_jimsh ${SQ}/autosetup/jimsh0.c)
target_compile_definitions(sqlite_jimsh PRIVATE HAVE_REALPATH)
set_target_properties(sqlite_jimsh PROPERTIES OUTPUT_NAME jimsh RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/sqlite)
add_custom_target(host_tools ALL DEPENDS nid_patcher pe_import_first glslang-standalone
    sqlite_lemon sqlite_mkkeywordhash sqlite_mksourceid sqlite_jimsh)
