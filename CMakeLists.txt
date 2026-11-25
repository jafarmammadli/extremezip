cmake_minimum_required(VERSION 3.25)
project(ExtremeExtract C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)

set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_COMPRESSION_SUPPORT OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_GZIP_SUPPORT ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_ZLIB_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libdeflate
  GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git
  GIT_TAG v1.24 GIT_SHALLOW TRUE)

set(ZLIB_COMPAT OFF CACHE BOOL "" FORCE)
set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
set(WITH_GZFILEOP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlibng
  GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
  GIT_TAG 2.2.4 GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(libdeflate zlibng)

add_library(xxengine STATIC src/zip.cpp src/inflate.cpp src/pinflate.cpp src/engine.cpp)
target_include_directories(xxengine PUBLIC src)
target_link_libraries(xxengine PUBLIC libdeflate::libdeflate_static zlib)
target_compile_definitions(xxengine PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(xxengine PUBLIC /W4 /permissive- /utf-8 $<$<CONFIG:Release>:/O2 /Oi /Gy>)

add_executable(xx src/cli.cpp)
target_link_libraries(xx PRIVATE xxengine)

add_executable(ExtremeExtract WIN32 src/gui.cpp src/gui.manifest)
target_link_libraries(ExtremeExtract PRIVATE xxengine)
