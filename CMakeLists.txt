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
find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)

# OBS itself also links FFmpeg, but there's no guarantee its own bundled
# avcodec/avformat/avutil DLLs are the exact same version this plugin was
# built against -- a mismatch isn't a version-skew warning, it's a silent
# LoadLibrary failure (Win32 error 126, "module not found") for a dependency
# that Windows can't locate anywhere by that exact filename, since a
# different-versioned DLL doesn't satisfy an import by name. Bundling our own
# copies next to the plugin removes the dependency on OBS's version entirely --
# Windows checks the loading DLL's own folder before anywhere else.
if(OS_WINDOWS)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:FFmpeg::avformat>" "$<TARGET_FILE:FFmpeg::avcodec>"
      "$<TARGET_FILE:FFmpeg::avutil>" "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
    COMMENT "Bundling FFmpeg runtime DLLs alongside ${CMAKE_PROJECT_NAME}"
    VERBATIM
  )
  install(FILES "$<TARGET_FILE:FFmpeg::avformat>" "$<TARGET_FILE:FFmpeg::avcodec>" "$<TARGET_FILE:FFmpeg::avutil>"
          DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit" COMPONENT Runtime)
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
