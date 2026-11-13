cmake_minimum_required(VERSION 3.25)

# Seamless Door and Travel Transitions: a modified version of LoadingFlow 1.2.0 (RandyLua,
# https://github.com/RandyLuaW/LoadingFlow), distributed under GPL-3.0 (LoadingFlow's alternative licence).
project(
	SeamlessTransitions
	VERSION 1.1.2
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep the build machine's directory names out of the binary.
get_filename_component(_src_parent "${CMAKE_SOURCE_DIR}" DIRECTORY)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	add_compile_options("/clang:-ffile-prefix-map=${_src_parent}/=")
	add_link_options("/PDBALTPATH:%_PDB%")
endif()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
# The released DLL targets the Anniversary Edition only. -DSEAMLESS_WITH_SE=ON also builds in support for
# Special Edition 1.5.97 (one DLL for both).
option(SEAMLESS_WITH_SE "Also support Skyrim SE 1.5.97" OFF)
set(ENABLE_SKYRIM_SE ${SEAMLESS_WITH_SE} CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

# MinHook 1.3.4 (BSD-2-Clause), vendored from Riftweaver's third_party: function-entry detours for the
# frame-generation pause (hooks the FidelityFX frame generation DLL's ffxConfigure).
enable_language(C)
add_library(minhook STATIC
	extern/minhook/src/buffer.c extern/minhook/src/hook.c extern/minhook/src/trampoline.c extern/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC extern/minhook/include PRIVATE extern/minhook/src)
target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN)

add_commonlibsse_plugin(
	${PROJECT_NAME}
	AUTHOR "zbratt (based on LoadingFlow by RandyLua)"
	USE_ADDRESS_LIBRARY
	SOURCES
		src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src include)
target_link_libraries(${PROJECT_NAME} PRIVATE minhook)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN LOADINGFLOW_WITH_MENU=1 IMGUI_DEFINE_MATH_OPERATORS)
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable)
