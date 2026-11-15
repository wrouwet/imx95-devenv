SUMMARY = "Board tmux console: Cortex-M7 RPMsg shell next to the Linux shell"
DESCRIPTION = "Starts a tmux session at boot with the M7's Zephyr shell (RPMsg) \
and a Linux shell; serial-console autologin and SSH logins attach to it, and \
Weston opens a terminal on it."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://imx95-console \
    file://imx95-console-attach \
    file://imx95-console-gui \
    file://m7-term \
    file://tmux.conf \
    file://imx95-console.service \
    file://serial-autologin.conf \
    file://zz-imx95-console.sh \
"
S = "${UNPACKDIR}"

inherit systemd allarch
SYSTEMD_SERVICE:${PN} = "imx95-console.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Serial console device(s) to log in automatically (from SERIAL_CONSOLES)
AUTOLOGIN_TTYS ?= "${@' '.join(c.split(';')[1] for c in d.getVar('SERIAL_CONSOLES').split())}"

do_install() {
    for f in imx95-console imx95-console-attach imx95-console-gui m7-term; do
        install -D -m 0755 ${S}/$f ${D}${bindir}/$f
    done
    install -D -m 0644 ${S}/tmux.conf ${D}${sysconfdir}/imx95-console/tmux.conf
    install -D -m 0644 ${S}/imx95-console.service ${D}${systemd_system_unitdir}/imx95-console.service
    install -D -m 0644 ${S}/zz-imx95-console.sh ${D}${sysconfdir}/profile.d/zz-imx95-console.sh
    for tty in ${AUTOLOGIN_TTYS}; do
        install -D -m 0644 ${S}/serial-autologin.conf \
            ${D}${systemd_system_unitdir}/serial-getty@$tty.service.d/autologin.conf
    done
}

FILES:${PN} += "${systemd_system_unitdir}/serial-getty@*.service.d"
RDEPENDS:${PN} = "tmux picocom m7-tools util-linux-agetty"
