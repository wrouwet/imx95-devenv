# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(m7_netipc)

target_include_directories(app PRIVATE ${LIBMETAL_INCLUDE_DIR} ${OPENAMP_INCLUDE_DIR} ${PLATFORM_DIR})
target_sources(app PRIVATE src/main.c src/rpmsg.c src/net.c)

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
