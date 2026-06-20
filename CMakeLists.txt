#
# Copyright (c) 2022 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf_wifi_sta)

target_include_directories(app PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${ZEPHYR_BASE}/subsys/net/ip
)

target_sources(app PRIVATE
	src/main.c
	src/wt_app.c
	src/wt_ble.c
	src/wt_common.c
	src/wt_leds.c
	src/wt_radio.c
	src/wt_shell.c
	src/wt_wifi.c
)
