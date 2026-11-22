cmake_minimum_required(VERSION 3.16)
project(usbcam-obs CXX)
set(CMAKE_CXX_STANDARD 17)
# Inputs (from the GitHub workflow): OBS_SRC = obs-studio source (headers), OBS_LIB = obs.lib made from the release's obs.dll,
# SIMDE_DIR = simde checkout (libobs headers need it), PTHREAD_DIR = obs-studio/deps/w32-pthreads, FFMPEG_DIR = FFmpeg shared build
add_library(usbcam-obs MODULE usbcam_obs.cpp)
set_target_properties(usbcam-obs PROPERTIES PREFIX "")
target_compile_definitions(usbcam-obs PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(usbcam-obs PRIVATE /utf-8)
target_include_directories(usbcam-obs PRIVATE ${OBS_STUB} ${OBS_SRC}/libobs ${SIMDE_DIR} ${PTHREAD_DIR} ${FFMPEG_DIR}/include)
target_link_directories(usbcam-obs PRIVATE ${FFMPEG_DIR}/lib)
target_link_libraries(usbcam-obs PRIVATE ${OBS_LIB} avcodec avutil ws2_32)
