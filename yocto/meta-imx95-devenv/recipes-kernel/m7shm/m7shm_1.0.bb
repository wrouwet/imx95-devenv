SUMMARY = "m7shm: memory window in the i.MX95 Cortex-M7 DTCM, shared with Linux"
DESCRIPTION = "Kernel module exposing the M7 TCM window as /dev/m7shm (mmap, read/write, \
in-kernel API) and following the M7's remoteproc state, plus m7shm-tool. \
The first 4 KiB of the window are the Wimmo virtual EHCI's register page. \
Autoloads from the devicetree node (devenv,imx95-m7shm)."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

# Sources live in the repo's linux/m7shm (outside the layer)
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../linux:"
SRC_URI = "file://m7shm/"
S = "${UNPACKDIR}/m7shm"

inherit module

MAKE_TARGETS = "modules"

do_compile:append() {
    ${CC} ${CFLAGS} ${LDFLAGS} -O2 -Wall -I${S} -o ${B}/m7shm-tool ${S}/tools/m7shm-tool.c
}

do_install:append() {
    install -D -m 0755 ${B}/m7shm-tool ${D}${bindir}/m7shm-tool
}

PACKAGES += "${PN}-tool"
FILES:${PN}-tool = "${bindir}/m7shm-tool"
RRECOMMENDS:${PN} += "${PN}-tool"
