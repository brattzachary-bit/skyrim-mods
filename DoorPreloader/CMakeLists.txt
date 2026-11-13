cmake_minimum_required(VERSION 3.25)

# Door Preloader: loads what lies behind a door before you open it.
project(
	DoorPreloader
	VERSION 1.1.1
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
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)
include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

add_commonlibsse_plugin(
	${PROJECT_NAME}
	AUTHOR "zbratt"
	USE_ADDRESS_LIBRARY
	SOURCES
		src/main.cpp
		src/Settings.cpp
		src/Index.cpp
		src/Preloader.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
