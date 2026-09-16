# Проект inkmetrics на ESP-IDF (см. PLAN-ONDEVICE.md, фаза Ф9)
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# собираем только то, что нужно проекту и его зависимостям
idf_build_set_property(MINIMAL_BUILD ON)
project(inkmetrics_idf)
