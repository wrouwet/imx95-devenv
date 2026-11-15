# Boot the imx95-devenv device tree by default (U-Boot env "fdtfile").
# If U-Boot already has a saved environment on the board, reset it once with:
#   => env default -a; saveenv
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append:mx95-nxp-bsp = " file://devenv-fdt.cfg"
