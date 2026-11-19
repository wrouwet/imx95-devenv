# SPDX-License-Identifier: Apache-2.0
zephyr_library()
# uhc_common.h is private to Zephyr's drivers/usb/uhc
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/uhc)
zephyr_library_sources(uhc_ci_ehci.c)
