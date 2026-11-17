SUMMARY = "USB network gadget (NCM) on the EVK Type-C port, with DHCP for the host"
DESCRIPTION = "Board 192.168.95.2 on usb0; systemd-networkd serves the host one \
DHCP lease (no router/DNS), so the link comes up with no host configuration. \
Forced to USB high-speed: SuperSpeed link training failed with the cable used \
during bring-up (the same problem broke uuu fastboot)."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://usb-gadget-net file://usb-gadget-net.service file://10-usb0.network"
S = "${UNPACKDIR}"

inherit systemd allarch
SYSTEMD_SERVICE:${PN} = "usb-gadget-net.service"

do_install() {
    install -D -m 0755 ${UNPACKDIR}/usb-gadget-net ${D}${bindir}/usb-gadget-net
    install -D -m 0644 ${UNPACKDIR}/usb-gadget-net.service ${D}${systemd_system_unitdir}/usb-gadget-net.service
    install -D -m 0644 ${UNPACKDIR}/10-usb0.network ${D}${sysconfdir}/systemd/network/10-usb0.network
}
RDEPENDS:${PN} = "kmod"
