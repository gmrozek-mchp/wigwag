# SPDX-License-Identifier: Apache-2.0

if(CONFIG_FLASH_PIC32CM_PL)
  zephyr_library_named(flash_pic32cm_pl)
  zephyr_library_sources(drivers/flash_pic32cm_pl.c)
endif()
