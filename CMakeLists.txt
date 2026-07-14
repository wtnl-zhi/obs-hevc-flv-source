cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swscale swresample)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    OBS::libobs
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
)

if(OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE winhttp)
elseif(OS_MACOS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "-framework CFNetwork" "-framework CoreFoundation")
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
