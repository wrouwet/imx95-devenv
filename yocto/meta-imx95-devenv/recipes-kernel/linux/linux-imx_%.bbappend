FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += " \
    file://imx95-19x19-evk-devenv.dts \
    file://devenv.cfg \
"

KERNEL_DEVICETREE:append:mx95-nxp-bsp = " freescale/imx95-19x19-evk-devenv.dtb"

# Drop the dts into the tree and register it with Kbuild (idempotent).
do_configure:prepend:mx95-nxp-bsp() {
    dtsdir=${S}/arch/arm64/boot/dts/freescale
    install -m 0644 ${UNPACKDIR}/imx95-19x19-evk-devenv.dts $dtsdir/
    grep -q 'imx95-19x19-evk-devenv.dtb' $dtsdir/Makefile || \
        echo 'dtb-$(CONFIG_ARCH_MXC) += imx95-19x19-evk-devenv.dtb' >> $dtsdir/Makefile
}
