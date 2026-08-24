cmake_minimum_required(VERSION 3.16)
project(cycles-vj-obs-plugin C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OBS források és könyvtárak elérési útja
if(NOT DEFINED OBS_SRC)
    set(OBS_SRC "C:/obs-src" CACHE PATH "Path to OBS Studio source")
endif()

if(NOT DEFINED OBS_LIB_DIR)
    set(OBS_LIB_DIR "C:/obs-dev" CACHE PATH "Path to OBS .lib directory")
endif()

# OBS include útvonalak hozzáadása
include_directories(
    ${OBS_SRC}/libobs
    src
)

# Forrásfájlok összegyűjtése a projektből
file(GLOB_RECURSE PLUGIN_SOURCES 
    "src/*.cpp" 
    "src/*.c" 
    "src/*.hpp" 
    "src/*.h"
)

# Ha a gyökérben vannak a források és nem az src mappában:
if(NOT PLUGIN_SOURCES)
    file(GLOB PLUGIN_SOURCES "*.cpp" "*.c" "*.hpp" "*.h")
endif()

# Plugin DLL létrehozása
add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

# Csatolás az obs.lib-hez
target_link_directories(${PROJECT_NAME} PRIVATE ${OBS_LIB_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE obs)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cycles-vj"
)
