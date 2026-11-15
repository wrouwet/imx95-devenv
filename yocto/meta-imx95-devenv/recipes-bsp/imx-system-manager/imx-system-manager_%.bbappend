# Add the imx95-devenv System Manager configuration.
#
# mx95devenv.cfg = NXP configs/other/mx95evkrpmsg.cfg (Linux may load, start and
# stop the M7 logical machine via remoteproc) plus: ENETC0 VF1 (NETC_VSI1) and
# its MSI/stream ID (NETC_LDID3) move from the AP to the M7, and the M7 gets
# read-only access to NETC_ECAM and NETC0. See docs/sm-mx95devenv-vs-mx95evkrpmsg.diff.
#
# Selected with SYSTEM_MANAGER_CONFIG = "mx95devenv" (set in local.conf by
# scripts/yocto-setup.sh) so that imx-boot picks up the matching binary too.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://mx95devenv.cfg"

do_configure:prepend() {
    install -m 0644 ${UNPACKDIR}/mx95devenv.cfg ${S}/configs/mx95devenv.cfg
}
