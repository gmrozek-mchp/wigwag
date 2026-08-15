# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Out-of-tree flash driver for PL10's NVMCTRL. A module rather than a patch to the pinned tree:
# `west update` reverts patches silently (firmware/patches/README.md), and this shape is what the
# upstream PR will be. Must come before find_package(Zephyr).
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/pic32cm-pl-nvmctrl)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wigwag)

target_sources(app PRIVATE
  src/main.c
  src/rnwf_at.c     # Zephyr-free core, also built for the host tests and the simulator
  src/rnwf_uart.c   # the Zephyr transport behind struct rnwf_at_io
  src/link.c        # link supervision: may the device believe what it shows?
  src/button.c      # pure debounce and press classification
  src/button_gpio.c # the polled GPIO binding
  src/lamp.c        # pure animation: (state, link, time) -> three levels
  src/lamp_pwm.c    # the Zephyr renderer: gamma, polarity, its own thread
  src/wdog.c        # pure liveness accounting: is every task that matters still beating?
  src/wdog_wdt.c    # the WDT peripheral, and the reset-cause report at boot
)
