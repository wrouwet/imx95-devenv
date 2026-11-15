SUMMARY = "Cortex-M7 control tools for the imx95-devenv image"
DESCRIPTION = "m7ctl (remoteproc load/start/stop/console), ENETC VF handoff to \
the M7, optional firmware autostart, and the on-target Zephyr setup script."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://m7ctl \
    file://m7-vf-handoff \
    file://m7-vf-handoff.service \
    file://m7-autostart.service \
    file://m7.default \
    file://imx95-zephyr-setup \
"
S = "${UNPACKDIR}"

inherit systemd allarch

SYSTEMD_SERVICE:${PN} = "m7-vf-handoff.service m7-autostart.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -D -m 0755 ${S}/m7ctl ${D}${bindir}/m7ctl
    install -D -m 0755 ${S}/imx95-zephyr-setup ${D}${bindir}/imx95-zephyr-setup
    install -D -m 0755 ${S}/m7-vf-handoff ${D}${sbindir}/m7-vf-handoff
    install -D -m 0644 ${S}/m7.default ${D}${sysconfdir}/default/m7
    install -D -m 0644 ${S}/m7-vf-handoff.service ${D}${systemd_system_unitdir}/m7-vf-handoff.service
    install -D -m 0644 ${S}/m7-autostart.service ${D}${systemd_system_unitdir}/m7-autostart.service
}

CONFFILES:${PN} = "${sysconfdir}/default/m7"
RDEPENDS:${PN} = "pciutils iproute2 picocom kmod"
