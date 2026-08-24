cmake_minimum_required(VERSION 3.16)
project(cycles_vj LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(OBS_SRC)
    include_directories("${OBS_SRC}/libobs")
endif()

if(OBS_LIB_DIR)
    link_directories("${OBS_LIB_DIR}")
endif()

add_library(cycles_vj MODULE "src/plugin.cpp")
target_include_directories(cycles_vj PRIVATE src "${OBS_SRC}/libobs")

if(WIN32)
    target_link_libraries(cycles_vj PRIVATE obs winmm)
    set_target_properties(cycles_vj PROPERTIES PREFIX "" OUTPUT_NAME "cycles_vj")
endif()
