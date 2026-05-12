cmake_minimum_required(VERSION 3.22.1)

project(velk_platform_android VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Defer everything to the velk-ui top-level CMakeLists; it guards desktop-only
# subdirs (glfw runtime plugin, samples) behind `if(NOT ANDROID)`.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_BINARY_DIR}/velk-ui")
