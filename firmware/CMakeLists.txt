cmake_minimum_required(VERSION 3.16)
option(HANDHELD_HWTEST "Build the screen/button diagnostic instead of the UI" OFF)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(solar_fast_handheld)
