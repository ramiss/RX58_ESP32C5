cmake_minimum_required(VERSION 3.22)
# Explicit version: otherwise ESP-IDF derives it from `git describe`, which
# fails the configure step in a repo with no commits yet.
set(PROJECT_VER "0.9.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rx58_esp32c5)
