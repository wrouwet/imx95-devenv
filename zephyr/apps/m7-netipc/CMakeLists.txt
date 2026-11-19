# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Out-of-tree drivers of this repo (USB2 EHCI host): zephyr/modules/devenv
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/devenv)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(m7_netipc)

target_include_directories(app PRIVATE ${LIBMETAL_INCLUDE_DIR} ${OPENAMP_INCLUDE_DIR} ${PLATFORM_DIR})
target_sources(app PRIVATE src/main.c src/rpmsg.c src/net.c src/reflector.c src/shm.c)
# Zephyr's own USB host stack on USB2 (off while the vEHCI translator owns USB2)
target_sources_ifdef(CONFIG_USB_HOST_STACK app PRIVATE src/usb.c)
target_sources_ifdef(CONFIG_APP_VEHCI app PRIVATE src/vehci_a55.c)

# Linux imx_rproc (fsl,imx95-cm7) takes the M7 boot address from an ELF section
# named ".interrupts" (MCUXpresso SDK naming) and falls back to 0 (TCM) if there
# is none. Zephyr's vector table section is "rom_start", so a DDR image would be
# started at address 0 and hang silently (seen on hardware 2026-11-16). Rename
# the section in the final ELF; segments and load addresses are unchanged.
# Zephyr's own post-build hooks are collected inside find_package(Zephyr),
# before this file's code runs, so use a separate target that runs after the
# final link. objcopy --rename-section is a no-op once the name is changed.
add_custom_target(imx_rproc_vectors ALL
  COMMAND ${CMAKE_OBJCOPY} --rename-section rom_start=.interrupts
          ${ZEPHYR_BINARY_DIR}/${KERNEL_ELF_NAME}
  COMMENT "Renaming rom_start -> .interrupts for Linux imx_rproc"
)
add_dependencies(imx_rproc_vectors ${logical_target_for_zephyr_elf})
