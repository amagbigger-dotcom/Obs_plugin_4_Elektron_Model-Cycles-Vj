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
)

# Csak a konkrét forrásfájlt adjuk hozzá, így a CMake belső fájljai nem kerülnek bele
add_library(${PROJECT_NAME} MODULE plugin.cpp)

target_link_directories(${PROJECT_NAME} PRIVATE ${OBS_LIB_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE 
    obs 
    winmm
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cycles-vj"
)
