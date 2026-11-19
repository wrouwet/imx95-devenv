# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources(vehci.c vehci_hub.c vehci_ctrl.c vehci_testdev.c)
zephyr_library_sources_ifdef(CONFIG_VEHCI_XLAT vehci_xlat.c)
