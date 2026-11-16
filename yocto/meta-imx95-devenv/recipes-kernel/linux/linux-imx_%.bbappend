FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += " \
    file://imx95-19x19-evk-devenv.dts \
    file://devenv.cfg \
"

# The dtbs registered below are added to KERNEL_DEVICETREE globally in
# yocto/local.conf.append (needed for IMAGE_BOOT_FILES). U-Boot picks the
# per-adapter variant at boot by probing I2C (see the u-boot bbappend).

# Drop the dts into the tree and register the dtbs with Kbuild (idempotent).
do_configure:prepend:mx95-nxp-bsp() {
    dtsdir=${S}/arch/arm64/boot/dts/freescale
    install -m 0644 ${UNPACKDIR}/imx95-19x19-evk-devenv.dts $dtsdir/
    if ! grep -q 'imx95-19x19-evk-devenv' $dtsdir/Makefile; then
        cat >> $dtsdir/Makefile <<'EOF'

# imx95-devenv
dtb-$(CONFIG_ARCH_MXC) += imx95-19x19-evk-devenv.dtb
imx95-19x19-evk-devenv-adv7535-dtbs := imx95-19x19-evk-devenv.dtb imx95-19x19-evk-adv7535.dtbo
dtb-$(CONFIG_ARCH_MXC) += imx95-19x19-evk-devenv-adv7535.dtb
imx95-19x19-evk-devenv-lt9611uxc-dtbs := imx95-19x19-evk-devenv.dtb imx95-19x19-evk-lt9611uxc.dtbo
dtb-$(CONFIG_ARCH_MXC) += imx95-19x19-evk-devenv-lt9611uxc.dtb
EOF
    fi
}
