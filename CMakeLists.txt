cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
project(rht_sensor)

if(NOT IDF_TARGET STREQUAL "esp32c6" AND NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "ClimaCarta supports only esp32c6 and esp32s3 targets")
endif()
