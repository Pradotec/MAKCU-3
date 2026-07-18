# Top-level ESP-IDF project file for the MAKCM Right MCU (hub-enabled USB host).
# Build with ESP-IDF v5.5.x:  idf.py set-target esp32s3 && idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(makcm_right_hub_host)
