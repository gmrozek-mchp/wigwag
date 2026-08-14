# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wigwag)

target_sources(app PRIVATE
  src/main.c
  src/rnwf_at.c     # Zephyr-free core, also built for the host tests and the simulator
  src/rnwf_uart.c   # the Zephyr transport behind struct rnwf_at_io
  src/link.c        # link supervision: may the device believe what it shows?
)
