cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")
find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swresample)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swresample)

# OBS itself also links FFmpeg, but there's no guarantee its own bundled
# avcodec/avformat/avutil DLLs are the exact same version this plugin was
# built against -- a mismatch isn't a version-skew warning, it's a silent
# LoadLibrary failure (Win32 error 126, "module not found") for a dependency
# that Windows can't locate anywhere by that exact filename, since a
# different-versioned DLL doesn't satisfy an import by name. Bundling our own
# copies next to the plugin removes the dependency on OBS's version entirely --
# Windows checks the loading DLL's own folder before anywhere else.
#
# swresample-N.dll specifically: avcodec-61.dll itself imports swresample by
# name (e.g. swresample-5.dll), so it's a transitive dependency of a
# dependency, not just a direct one -- easy to miss by only looking at what
# replay-slider.dll itself imports. Missing it produces the exact same error
# 126 as the other three, because Windows fails the whole load chain the
# moment any one link (however deep) can't be resolved. Confirmed as the
# actual last missing piece via a real end-user machine whose OBS install
# only shipped swresample-6.dll, not the swresample-5.dll our avcodec build
# needs -- OBS's own copy doesn't help if it's the wrong SONAME version, same
# as avcodec/avformat/avutil before it.
if(OS_WINDOWS)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:FFmpeg::avformat>" "$<TARGET_FILE:FFmpeg::avcodec>"
      "$<TARGET_FILE:FFmpeg::avutil>" "$<TARGET_FILE:FFmpeg::swresample>" "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
    COMMENT "Bundling FFmpeg runtime DLLs alongside ${CMAKE_PROJECT_NAME}"
    VERBATIM
  )
  install(FILES "$<TARGET_FILE:FFmpeg::avformat>" "$<TARGET_FILE:FFmpeg::avcodec>" "$<TARGET_FILE:FFmpeg::avutil>"
                "$<TARGET_FILE:FFmpeg::swresample>"
          DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit" COMPONENT Runtime)

  # Same reasoning, different dependency: replay-slider.dll also links the
  # MSVC C/C++ runtime (MSVCP140.dll, VCRUNTIME140.dll, VCRUNTIME140_1.dll),
  # which is only guaranteed present if the end user's machine already has the
  # matching Visual C++ 2015-2022 Redistributable installed. A dev machine
  # (or anything that's ever installed Visual Studio, a game using the same
  # runtime, etc.) almost always has it already, silently masking this as a
  # problem -- a "bare" end-user PC that's never needed it does not, and hits
  # the exact same LoadLibrary error 126 the FFmpeg DLLs did. CMake's own
  # InstallRequiredSystemLibraries resolves the correct DLLs for whatever
  # toolchain actually built this, rather than hardcoding filenames/versions.
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  include(InstallRequiredSystemLibraries)
  if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    add_custom_command(
      TARGET ${CMAKE_PROJECT_NAME}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
      COMMENT "Bundling the MSVC runtime alongside ${CMAKE_PROJECT_NAME}"
      VERBATIM
    )
    install(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit" COMPONENT Runtime)
  endif()
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp
          src/replay-buffer-dock.cpp
          src/replay-buffer-dock.hpp
          src/hotkey-lookup.cpp
          src/hotkey-lookup.hpp
          src/clip-trim.cpp
          src/clip-trim.hpp
          src/websocket-bridge.cpp
          src/websocket-bridge.hpp
          src/obs-websocket-api.h)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
