#
# SuperMini nRF52840 Thread Network Health Checker
# Copyright (c) 2026 Antigravity
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(supermini_thread_health_checker)

target_sources(app PRIVATE
    src/main.cpp
    src/thread_health.cpp
)

target_include_directories(app PRIVATE src)
