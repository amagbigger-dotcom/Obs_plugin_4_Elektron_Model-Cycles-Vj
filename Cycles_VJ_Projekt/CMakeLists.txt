cmake_minimum_required(VERSION 3.16)
project(cycles-vj-plugins C CXX)

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

# plugin.cpp felkutatása
file(GLOB_RECURSE SRC_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "plugin.cpp" "src/plugin.cpp")
list(GET SRC_FILES 0 MAIN_SRC)
message(STATUS "Forrasfajl a forditasokhoz: ${MAIN_SRC}")

# A 22 kulonallo plugin neve
set(PLUGINS
    vz_2tonr
    vz_blurr
    vz_brcosr
    vz_breathr
    vz_clrmapr
    vz_cropr
    vz_exposr
    vz_fisheyr
    vz_hueshiftr
    vz_kaleidr
    vz_pinchr
    vz_pixel8r
    vz_rgbr
    vz_scanlines
    vz_slicr
    vz_sprinklr
    vz_strobr
    vz_twistr
    vz_zoropr
    vz_drtyfeedr
    vz_feedr
    vz_wavr
)

# Mind a 22 DLL letrehozasa a megfelelo BUILD_ makroval
foreach(PLUGIN_NAME ${PLUGINS})
    string(TOUPPER ${PLUGIN_NAME} UPPER_NAME)
    set(DEF_FLAG "BUILD_${UPPER_NAME}")
    
    add_library(${PLUGIN_NAME} MODULE ${MAIN_SRC})
    target_compile_definitions(${PLUGIN_NAME} PRIVATE ${DEF_FLAG})
    target_link_directories(${PLUGIN_NAME} PRIVATE ${OBS_LIB_DIR})
    target_link_libraries(${PLUGIN_NAME} PRIVATE obs winmm)
    
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        PREFIX ""
        OUTPUT_NAME ${PLUGIN_NAME}
    )
    message(STATUS "DLL cel regisztralva: ${PLUGIN_NAME} (${DEF_FLAG})")
endforeach()
