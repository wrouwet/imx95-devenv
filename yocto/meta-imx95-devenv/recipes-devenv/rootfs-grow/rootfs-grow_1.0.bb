SUMMARY = "Grow the root filesystem to the full boot device on first boot"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://rootfs-grow file://rootfs-grow.service"
S = "${UNPACKDIR}"

inherit systemd allarch
SYSTEMD_SERVICE:${PN} = "rootfs-grow.service"

do_install() {
    install -D -m 0755 ${S}/rootfs-grow ${D}${sbindir}/rootfs-grow
    install -D -m 0644 ${S}/rootfs-grow.service ${D}${systemd_system_unitdir}/rootfs-grow.service
}

RDEPENDS:${PN} = "e2fsprogs-resize2fs util-linux-sfdisk util-linux-partx util-linux-lsblk util-linux-findmnt"
