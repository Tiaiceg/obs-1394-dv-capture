cmake_minimum_required(VERSION 3.16)

# This plugin is Windows-only: IEEE-1394 DV capture is implemented through
# DirectShow and the Microsoft DV camera stack.
if(NOT WIN32)
  message(FATAL_ERROR
    "obs-dv-capture supports Windows only (IEEE-1394/DirectShow).")
endif()

project(obs-dv-capture
  VERSION 1.0.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Statically link the MSVC runtime so the plugin does not require a matching
# Visual C++ redistributable on the target machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY
  "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Pull in obs-studio (only libobs is built; no Qt/UI, no bundled plugins).
# ---------------------------------------------------------------------------
set(ENABLE_UI        OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PLUGINS   OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST      OFF CACHE BOOL "" FORCE)
set(ENABLE_HEVC      OFF CACHE BOOL "" FORCE)
set(ENABLE_VLC       OFF CACHE BOOL "" FORCE)
set(ENABLE_BROWSER   OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
  obs-studio
  GIT_REPOSITORY https://github.com/obsproject/obs-studio.git
  GIT_TAG        30.1.2
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(obs-studio)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
add_library(obs-dv-capture MODULE
  src/plugin-main.cpp
  src/dshow-capture.cpp)

target_include_directories(obs-dv-capture PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(obs-dv-capture PRIVATE
  OBS::libobs
  strmiids    # DirectShow class/interface GUIDs
  ole32       # COM
  oleaut32    # Variant helpers
)

# libobs plugins are plain ".dll" without the "lib" prefix.
set_target_properties(obs-dv-capture PROPERTIES
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden)

if(MSVC)
  target_compile_options(obs-dv-capture PRIVATE
    /utf-8
    /W3
    /permissive-)
  target_compile_definitions(obs-dv-capture PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    STRSAFE_NO_DEPRECATE)
endif()

# ---------------------------------------------------------------------------
# Install layout (portable, mirrors an OBS installation root):
#   obs-plugins/64bit/obs-dv-capture.dll
#   data/obs-plugins/obs-dv-capture/locale/en-US.ini
# ---------------------------------------------------------------------------
install(TARGETS obs-dv-capture
  RUNTIME DESTINATION obs-plugins/64bit
  LIBRARY DESTINATION obs-plugins/64bit)

install(FILES resources/locale/en-US.ini
  DESTINATION data/obs-plugins/obs-dv-capture/locale)
