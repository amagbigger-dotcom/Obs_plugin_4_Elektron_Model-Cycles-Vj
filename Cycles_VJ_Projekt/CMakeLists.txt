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

# Megkeressuk a plugin.cpp-t
file(GLOB_RECURSE SRC_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "plugin.cpp" "src/plugin.cpp")
list(GET SRC_FILES 0 MAIN_SRC)

# A 22 plugin neve es fordítási flagje
set(PLUGINS
    "vz_2tonr;BUILD_VZ_2TONR"
    "vz_blurr;BUILD_VZ_BLURR"
    "vz_brcosr;BUILD_VZ_BRCOSR"
    "vz_breathr;BUILD_VZ_BREATHR"
    "vz_clrmapr;BUILD_VZ_CLRMAPR"
    "vz_cropr;BUILD_VZ_CROPR"
    "vz_exposr;BUILD_VZ_EXPOSR"
    "vz_fisheyr;BUILD_VZ_FISHEYR"
    "vz_hueshiftr;BUILD_VZ_HUESHIFTR"
    "vz_kaleidr;BUILD_VZ_KALEIDR"
    "vz_pinchr;BUILD_VZ_PINCHR"
    "vz_pixel8r;BUILD_VZ_PIXEL8R"
    "vz_rgbr;BUILD_VZ_RGBR"
    "vz_scanlines;BUILD_VZ_SCANLINES"
    "vz_slicr;BUILD_VZ_SLICR"
    "vz_sprinklr;BUILD_VZ_SPRINKLR"
    "vz_strobr;BUILD_VZ_STROBR"
    "vz_twistr;BUILD_VZ_TWISTR"
    "vz_zoropr;BUILD_VZ_ZOROPR"
    "vz_drtyfeedr;BUILD_VZ_DRTYFEEDR"
    "vz_feedr;BUILD_VZ_FEEDR"
    "vz_wavr;BUILD_VZ_WAVR"
)

foreach(ITEM ${PLUGINS})
    list(GET ITEM 0 TARGET_NAME)
    list(GET ITEM 1 DEF_FLAG)
    
    add_library(${TARGET_NAME} MODULE ${MAIN_SRC})
    target_compile_definitions(${TARGET_NAME} PRIVATE ${DEF_FLAG})
    target_link_directories(${TARGET_NAME} PRIVATE ${OBS_LIB_DIR})
    target_link_libraries(${TARGET_NAME} PRIVATE obs winmm)
    
    set_target_properties(${TARGET_NAME} PROPERTIES
        PREFIX ""
        OUTPUT_NAME ${TARGET_NAME}
    )
endforeach()
