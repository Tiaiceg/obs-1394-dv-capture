cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# IEEE-1394 DV capture is implemented through DirectShow, which is
# Windows-only.
if(NOT WIN32)
  message(FATAL_ERROR "obs-dv-capture supports Windows only (DirectShow).")
endif()

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/plugin-main.cpp
  src/dshow-capture.cpp
  src/dshow-capture.h
  src/qedit-defs.h)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# DirectShow / Windows system libraries.
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
  strmiids
  ole32
  oleaut32)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  STRSAFE_NO_DEPRECATE)

if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /utf-8)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
