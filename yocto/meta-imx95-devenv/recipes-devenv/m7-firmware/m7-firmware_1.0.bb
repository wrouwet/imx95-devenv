SUMMARY = "Default Cortex-M7 Zephyr firmware (m7-netipc) for the imx95-devenv image"
DESCRIPTION = "Installs a Zephyr ELF built outside Yocto (scripts/zephyr-build.sh) \
as /lib/firmware/imx95-m7-zephyr.elf, started at boot by m7-autostart.service. \
Set M7_FIRMWARE_ELF in local.conf; if the file doesn't exist the package is empty \
and the M7 simply isn't auto-started."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

M7_FIRMWARE_ELF ??= ""

python __anonymous() {
    import os
    elf = d.getVar('M7_FIRMWARE_ELF')
    if elf and os.path.isfile(elf):
        d.appendVar('SRC_URI', ' file://' + elf)
        d.setVar('M7_ELF_PRESENT', '1')
    else:
        bb.warn("m7-firmware: M7_FIRMWARE_ELF='%s' not found; image will have no default M7 firmware" % elf)
        d.setVar('M7_ELF_PRESENT', '')
}

S = "${UNPACKDIR}"
inherit allarch
# The ELF is for the Cortex-M7, not the A55 - don't strip or QA-check it as such.
INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_SYSROOT_STRIP = "1"
INSANE_SKIP:${PN} = "arch ldflags textrel already-stripped"
ALLOW_EMPTY:${PN} = "1"

do_install() {
    if [ -n "${M7_ELF_PRESENT}" ]; then
        install -D -m 0644 "${UNPACKDIR}/${M7_FIRMWARE_ELF}" ${D}${nonarch_base_libdir}/firmware/imx95-m7-zephyr.elf
    fi
}
FILES:${PN} = "${nonarch_base_libdir}/firmware/imx95-m7-zephyr.elf"
do_install[file-checksums] += "${@d.getVar('M7_FIRMWARE_ELF') + ':True' if d.getVar('M7_ELF_PRESENT') else ''}"
