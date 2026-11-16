SUMMARY = "imx95-devenv developer image for the i.MX95 19x19 EVK"
DESCRIPTION = "Weston/Wayland image (headless works too) with SSH, a native \
toolchain, networking/debug tools, Python/west/cmake/ninja/dtc so Zephyr can be \
built for the M7 on the board, M7 remoteproc/RPMsg tools, Ethernet sharing \
(ENETC VF handoff to the M7) and a tmux console showing the M7 next to Linux."
LICENSE = "MIT"

inherit core-image

IMAGE_FEATURES += " \
    ssh-server-openssh \
    tools-sdk \
    tools-debug \
    tools-profile \
    package-management \
    hwcodecs \
    ${@bb.utils.contains('DISTRO_FEATURES', 'wayland', 'weston', '', d)} \
"

# --- Access without passwords ---
# - Serial console (debug USB): automatic root login (imx95-console package).
# - SSH: public-key only. The key in DEVENV_SSH_PUBKEY (generated on the build
#   host by scripts/yocto-setup.sh) is installed for root. Password and
#   empty-password logins are disabled; root has no password at all.
# NXP's default local.conf sets EXTRA_IMAGE_FEATURES to "allow-empty-password
# empty-root-password allow-root-login ...", which leaves root with an EMPTY
# password and rewrites sshd_config to PermitEmptyPasswords/PermitRootLogin yes.
# Strip those; root keeps a locked password ("*") and logs in by key only
# (PermitRootLogin prohibit-password, set below).
IMAGE_FEATURES:remove = "allow-empty-password empty-root-password allow-root-login"
DEVENV_SSH_PUBKEY ??= ""

ROOTFS_POSTPROCESS_COMMAND += "devenv_ssh_setup;"
devenv_ssh_setup() {
    cfg=${IMAGE_ROOTFS}${sysconfdir}/ssh/sshd_config
    if [ -f "$cfg" ]; then
        sed -i -e '/^#\?PasswordAuthentication/d' -e '/^#\?KbdInteractiveAuthentication/d' \
               -e '/^#\?ChallengeResponseAuthentication/d' -e '/^#\?PermitEmptyPasswords/d' \
               -e '/^#\?PermitRootLogin/d' -e '/^#\?PubkeyAuthentication/d' "$cfg"
        cat >> "$cfg" <<EOF

# imx95-devenv: key-only SSH
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitEmptyPasswords no
PermitRootLogin prohibit-password
EOF
    fi
    if [ -n "${DEVENV_SSH_PUBKEY}" ] && [ -f "${DEVENV_SSH_PUBKEY}" ]; then
        install -d -m 0700 ${IMAGE_ROOTFS}${ROOT_HOME}/.ssh
        install -m 0600 "${DEVENV_SSH_PUBKEY}" ${IMAGE_ROOTFS}${ROOT_HOME}/.ssh/authorized_keys
    else
        bbwarn "DEVENV_SSH_PUBKEY not set/found: SSH login will be impossible (serial console still works)"
    fi
}
do_rootfs[file-checksums] += "${@d.getVar('DEVENV_SSH_PUBKEY') + ':True' if d.getVar('DEVENV_SSH_PUBKEY') else ''}"

IMAGE_INSTALL += " \
    packagegroup-core-full-cmdline \
    kernel-modules \
    kernel-devsrc \
    m7-tools \
    m7-firmware \
    imx95-console \
    rootfs-grow \
"

# Build tools: native gcc/g++/make etc. come from tools-sdk; these add the
# Zephyr/west flow and common build systems (kernel-devsrc: out-of-tree modules).
IMAGE_INSTALL += " \
    git git-perltools cmake ninja meson pkgconfig dtc bc bison flex \
    python3 python3-pip python3-venv python3-pyelftools python3-pyyaml \
    python3-packaging python3-pykwalify python3-jsonschema python3-colorama \
    python3-requests python3-setuptools python3-pyserial \
    xz zstd file rsync curl wget tar gzip unzip patch diffutils \
"

# Rust: native rustc/cargo on the board (+ clippy, rustfmt), by user request
IMAGE_INSTALL += "packagegroup-rust-sdk-target rust-tools-clippy rust-tools-rustfmt ripgrep"

# Networking: config, debugging, performance, PTP and PHY access
IMAGE_INSTALL += " \
    ethtool iproute2 iproute2-tc iputils net-tools bridge-utils \
    tcpdump tcpreplay nmap netcat-openbsd socat traceroute mtr \
    iperf3 iperf2 lldpd linuxptp phytool mdio-tools \
    nftables openssh-sftp-server chrony \
"

# Editors, shells, system and hardware debugging
IMAGE_INSTALL += " \
    vim nano tmux screen picocom minicom bash-completion less tree jq ncdu \
    htop iotop sysstat lsof strace ltrace valgrind \
    pciutils usbutils i2c-tools spitools can-utils libgpiod-tools devmem2 \
    memtester stress-ng evtest ser2net mmc-utils parted e2fsprogs-resize2fs util-linux \
"

# Room to clone and build Zephyr on the board before rootfs-grow expands
# the root partition to the full eMMC/SD size on first boot.
IMAGE_ROOTFS_EXTRA_SPACE = "2097152"
