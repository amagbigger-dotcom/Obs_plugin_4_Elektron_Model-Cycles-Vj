cmake_minimum_required(VERSION 3.16)
project(cycles-vj-obs-plugin C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED OBS_SRC)
    set(OBS_SRC "C:/obs-src" CACHE PATH "Path to OBS Studio source")
endif()

if(NOT DEFINED OBS_LIB_DIR)
    set(OBS_LIB_DIR "C:/obs-dev" CACHE PATH "Path to OBS .lib directory")
endif()

include_directories(
    ${OBS_SRC}/libobs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# plugin.cpp megkeresése (kizárva a build mappát)
file(GLOB_RECURSE PLUGIN_SOURCES 
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    "*.cpp" "*.c"
)
list(FILTER PLUGIN_SOURCES EXCLUDE REGEX "^build/.*")
list(FILTER PLUGIN_SOURCES EXCLUDE REGEX "^CMakeFiles/.*")
list(FILTER PLUGIN_SOURCES EXCLUDE REGEX "CMake.*CompilerId.*")

if(NOT PLUGIN_SOURCES)
    message(FATAL_ERROR "Nem talalhato forrasfajl (plugin.cpp)!")
endif()

message(STATUS "Forrasfajlok: ${PLUGIN_SOURCES}")

add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

target_link_directories(${PROJECT_NAME} PRIVATE ${OBS_LIB_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE 
    obs 
    winmm
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cycles-vj"
)
