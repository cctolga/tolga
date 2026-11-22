cmake_minimum_required(VERSION 3.20)
project(Tolga VERSION 0.1.14 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)   # must come right after project(), before any target (Ninja needs it)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# ---------------------------------------------------------------- SDL3
# Use a system SDL3 (e.g. `brew install sdl3`) if present, otherwise build it.
# Release bundles set TOLGA_SYSTEM_SDL=OFF so SDL is linked statically (nothing from Homebrew).
option(TOLGA_SYSTEM_SDL "Use an installed SDL3 if there is one" ON)
if(TOLGA_SYSTEM_SDL)
  find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND)
  message(STATUS "SDL3 not found on system, fetching it")
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.20
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(SDL3)
endif()

# ---------------------------------------------------------------- Dear ImGui
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.9b
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

# JSON for the OpenSubtitles API (single header)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

# HTTPS client for online subtitle search (macOS ships libcurl; Linux: libcurl4-openssl-dev)
find_package(CURL REQUIRED)

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3 OpenGL::GL)

# ---------------------------------------------------------------- libmpv
# macOS: `brew install mpv pkg-config`
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
# the same FFmpeg libraries mpv uses: reading picture subtitles to turn them into text
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)

# ---------------------------------------------------------------- AirPlay
# AirPlay is a separate, closed-source module; the player only knows the interface in
# include/tolga/AirPlayApi.h. Where it comes from, in this order:
#   1. TOLGA_AIRPLAY_DIR: the module's private source folder (the author's builds)
#   2. the prebuilt library (macOS, Linux x86_64, Windows x86_64): downloaded once from this repository's releases and
#      checked against the SHA-256 in cmake/AirPlayPrebuilt.cmake. AirPlay then works for a
#      14-day trial and afterwards needs a Tolga license.
#      TOLGA_AIRPLAY_PREBUILT_FILE uses a local package instead (any OS, for testing).
#   3. none: src/AirPlayNone.cpp, the AirPlay menu points to the official app.
set(TOLGA_AIRPLAY_DIR "" CACHE PATH "Source folder of the Tolga AirPlay module")
option(TOLGA_AIRPLAY_PREBUILT "Download the prebuilt AirPlay module (macOS, Linux x86_64)" ON)
set(TOLGA_AIRPLAY_PREBUILT_FILE "" CACHE FILEPATH "Local prebuilt AirPlay package (.tar.gz) to use instead of downloading")
set(TOLGA_WEBSITE "https://cctolga.github.io/tolga/" CACHE STRING "Web page of the official Tolga app")
set(TOLGA_API_INCLUDE ${CMAKE_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
include(cmake/AirPlayPrebuilt.cmake)
# the package for this platform (empty if none is published for it)
if(APPLE)
  set(_prebuilt_key macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(_prebuilt_key linux_x86_64)
elseif(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(_prebuilt_key windows_x86_64)
else()
  set(_prebuilt_key none)
endif()
foreach(_f VERSION API URL SHA256)
  set(TOLGA_AIRPLAY_PREBUILT_${_f} "${TOLGA_AIRPLAY_PREBUILT_${_prebuilt_key}_${_f}}")
endforeach()
file(STRINGS ${TOLGA_API_INCLUDE}/tolga/AirPlayApi.h _api_line REGEX "^#define TOLGA_AIRPLAY_API_VERSION")
string(REGEX REPLACE ".* ([0-9]+).*" "\\1" _api_version "${_api_line}")

set(_airplay_lib "")
if(TOLGA_AIRPLAY_DIR)
  add_subdirectory(${TOLGA_AIRPLAY_DIR} ${CMAKE_BINARY_DIR}/airplay-module)
  set(_airplay_lib tolga_airplay_module)
  message(STATUS "AirPlay: module built from ${TOLGA_AIRPLAY_DIR}")
elseif(TOLGA_AIRPLAY_PREBUILT AND (TOLGA_AIRPLAY_PREBUILT_FILE OR TOLGA_AIRPLAY_PREBUILT_URL))
  if(APPLE)
    set(_prebuilt_name libtolga_airplay.dylib)
    set(_prebuilt_soname "@rpath/libtolga_airplay.dylib")
  elseif(WIN32)
    set(_prebuilt_name tolga_airplay.dll)
    set(_prebuilt_soname "")
  else()
    set(_prebuilt_name libtolga_airplay.so)
    set(_prebuilt_soname libtolga_airplay.so)
  endif()
  set(_pkg "")
  if(TOLGA_AIRPLAY_PREBUILT_FILE)
    set(_pkg "${TOLGA_AIRPLAY_PREBUILT_FILE}")
    set(_dir ${CMAKE_BINARY_DIR}/airplay-prebuilt-local)
  elseif(NOT TOLGA_AIRPLAY_PREBUILT_API EQUAL _api_version)
    message(WARNING "AirPlay: the prebuilt module is for API ${TOLGA_AIRPLAY_PREBUILT_API}, this source has "
                    "${_api_version}; building without AirPlay until a matching module is published")
  else()
    set(_dir ${CMAKE_BINARY_DIR}/airplay-prebuilt-${TOLGA_AIRPLAY_PREBUILT_VERSION})
    set(_pkg ${_dir}.tar.gz)
    if(NOT EXISTS ${_dir}/lib/${_prebuilt_name})
      message(STATUS "AirPlay: downloading the prebuilt module ${TOLGA_AIRPLAY_PREBUILT_VERSION}")
      file(DOWNLOAD ${TOLGA_AIRPLAY_PREBUILT_URL} ${_pkg} TLS_VERIFY ON STATUS _st
           EXPECTED_HASH SHA256=${TOLGA_AIRPLAY_PREBUILT_SHA256})
      list(GET _st 0 _code)
      if(NOT _code EQUAL 0)
        list(GET _st 1 _msg)
        message(WARNING "AirPlay: could not download the prebuilt module (${_msg}); building without AirPlay")
        file(REMOVE ${_pkg})
        set(_pkg "")
      endif()
    endif()
  endif()
  if(_pkg AND NOT EXISTS ${_dir}/lib/${_prebuilt_name})
    file(REMOVE_RECURSE ${_dir})
    file(ARCHIVE_EXTRACT INPUT ${_pkg} DESTINATION ${_dir}.x)
    file(GLOB _inner LIST_DIRECTORIES true ${_dir}.x/*)
    file(RENAME ${_inner} ${_dir})
    file(REMOVE_RECURSE ${_dir}.x)
  endif()
  if(EXISTS ${_dir}/lib/${_prebuilt_name})
    # a shared library that exports only createAirPlay(); found next to the app at run time
    add_library(tolga_airplay_prebuilt SHARED IMPORTED)
    set_target_properties(tolga_airplay_prebuilt PROPERTIES
      IMPORTED_LOCATION ${_dir}/lib/${_prebuilt_name}
      IMPORTED_SONAME "${_prebuilt_soname}"
      INTERFACE_INCLUDE_DIRECTORIES ${TOLGA_API_INCLUDE})
    if(WIN32)  # linked through its import library, the DLL goes next to the .exe
      set_target_properties(tolga_airplay_prebuilt PROPERTIES IMPORTED_IMPLIB ${_dir}/lib/libtolga_airplay.dll.a)
    endif()
    set(_airplay_lib tolga_airplay_prebuilt)
    set(TOLGA_AIRPLAY_PREBUILT_DIR ${_dir} CACHE INTERNAL "")
    message(STATUS "AirPlay: prebuilt closed-source module (${_dir})")
  endif()
endif()
if(NOT _airplay_lib)
  add_library(tolga_airplay_none STATIC src/AirPlayNone.cpp)
  target_include_directories(tolga_airplay_none PUBLIC ${TOLGA_API_INCLUDE})
  set(_airplay_lib tolga_airplay_none)
  message(STATUS "AirPlay: not included")
endif()

# ---------------------------------------------------------------- app
add_executable(tolga
  src/main.cpp
  src/OpenSubtitles.cpp
  src/DurationCache.cpp
  src/Thumbnailer.cpp
  src/RecentFiles.cpp
  src/ResumePositions.cpp
  src/SubtitleOcr.cpp
  src/MpvPlayer.cpp
  src/Overlay.cpp
  src/Ffmpeg.cpp
  src/DefaultApp.cpp
  src/WindowChrome.cpp
  src/I18n.cpp)
target_include_directories(tolga PRIVATE ${TOLGA_API_INCLUDE})

# Translations: resources/lang/<code>.json are built into the program (src/I18n.cpp)
file(GLOB _lang_files CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/resources/lang/*.json)
set(_lang_inc "")
foreach(_f ${_lang_files})
  get_filename_component(_code ${_f} NAME_WE)
  file(READ ${_f} _json)
  string(APPEND _lang_inc "    {\"${_code}\", R\"TOLGALANG(${_json})TOLGALANG\"},\n")
endforeach()
file(WRITE ${CMAKE_BINARY_DIR}/lang/Languages.inc.new "${_lang_inc}")
configure_file(${CMAKE_BINARY_DIR}/lang/Languages.inc.new ${CMAKE_BINARY_DIR}/lang/Languages.inc COPYONLY)
target_include_directories(tolga PRIVATE ${CMAKE_BINARY_DIR}/lang)
target_link_libraries(tolga PRIVATE imgui PkgConfig::MPV PkgConfig::LIBAV ${_airplay_lib} CURL::libcurl nlohmann_json::nlohmann_json
                      Threads::Threads)
target_compile_definitions(tolga PRIVATE TOLGA_WEBSITE="${TOLGA_WEBSITE}" TOLGA_VERSION="${PROJECT_VERSION}"
                          TOLGA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# OpenSubtitles API key built into the app, so users are never asked for one.
# Put the key in a file named "opensubtitles.key" next to this CMakeLists.txt (it is in
# .gitignore, so it never ends up in the repository), or pass -DTOLGA_OPENSUBTITLES_KEY=...
# Without a key, "Search Online" asks the user for their own (free) OpenSubtitles API key.
set(TOLGA_OPENSUBTITLES_KEY "" CACHE STRING "OpenSubtitles.com API key built into the app")
set(_os_key "${TOLGA_OPENSUBTITLES_KEY}")
if(_os_key STREQUAL "" AND EXISTS "${CMAKE_SOURCE_DIR}/opensubtitles.key")
  file(READ "${CMAKE_SOURCE_DIR}/opensubtitles.key" _os_key)
  string(STRIP "${_os_key}" _os_key)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/opensubtitles.key")
endif()
if(_os_key STREQUAL "")
  message(STATUS "OpenSubtitles: no built-in API key (users enter their own)")
else()
  message(STATUS "OpenSubtitles: built-in API key from the build")
endif()
target_compile_definitions(tolga PRIVATE TOLGA_OPENSUBTITLES_KEY="${_os_key}")

if(WIN32)
  # a window app (no console), "Tolga Player.exe" with the icon and version info
  set_target_properties(tolga PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME "Tolga Player")
  set(TOLGA_RC_DIR ${CMAKE_SOURCE_DIR}/resources/windows)
  configure_file(resources/windows/tolga.rc.in ${CMAKE_BINARY_DIR}/tolga.rc @ONLY)
  target_sources(tolga PRIVATE ${CMAKE_BINARY_DIR}/tolga.rc)
  set_source_files_properties(${CMAKE_BINARY_DIR}/tolga.rc PROPERTIES
    OBJECT_DEPENDS "${TOLGA_RC_DIR}/tolga.ico;${TOLGA_RC_DIR}/tolga.manifest")
  target_link_options(tolga PRIVATE -static-libgcc -static-libstdc++)
  target_link_libraries(tolga PRIVATE shlwapi dwmapi)   # AssocQueryString: who opens .mkv; dark title bar
endif()

if(NOT APPLE)
  target_sources(tolga PRIVATE src/SubtitleOcrNone.cpp)   # no text recognition here yet
endif()

if(APPLE)
  target_sources(tolga PRIVATE src/MacWindow.mm src/DefaultAppMac.mm src/SubtitleOcrMac.mm)
  set_source_files_properties(src/SubtitleOcrMac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(tolga PRIVATE "-framework Cocoa" "-framework CoreServices" "-framework UniformTypeIdentifiers"
                        "-framework Vision" "-framework CoreGraphics")
  target_compile_definitions(tolga PRIVATE GL_SILENCE_DEPRECATION)
  target_compile_definitions(imgui PRIVATE GL_SILENCE_DEPRECATION)
endif()
