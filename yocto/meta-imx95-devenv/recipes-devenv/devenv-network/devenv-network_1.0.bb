SUMMARY = "imx95-devenv network defaults: IPv4 link-local fallback on wired ports"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://10-link-local.conf"
S = "${UNPACKDIR}"

inherit allarch

do_install() {
    install -D -m 0644 ${UNPACKDIR}/10-link-local.conf \
        ${D}${sysconfdir}/systemd/network/80-wired.network.d/10-link-local.conf
}
FILES:${PN} = "${sysconfdir}/systemd/network"
