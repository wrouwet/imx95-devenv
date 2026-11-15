SUMMARY = "imx95-devenv developer image for the i.MX95 19x19 EVK"
DESCRIPTION = "Weston/Wayland image (headless works too) with SSH, a native \
toolchain, Python/west/cmake/ninja/dtc so Zephyr can be built for the M7 on \
the board itself, M7 remoteproc/RPMsg tools and Ethernet sharing (ENETC VF \
handoff to the M7)."
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

# Dev-board convenience: root login without password (serial + SSH).
# Set a root password (passwd) or install an SSH key on first login, and see
# README "Security" before putting the board on an untrusted network.
IMAGE_FEATURES += "allow-empty-password empty-root-password allow-root-login post-install-logging"

IMAGE_INSTALL += " \
    packagegroup-core-full-cmdline \
    kernel-modules \
    m7-tools \
    m7-firmware \
    rootfs-grow \
    \
    git git-perltools cmake ninja dtc python3 python3-pip python3-venv \
    python3-pyelftools python3-pyyaml python3-packaging python3-pykwalify \
    python3-jsonschema python3-colorama python3-requests python3-setuptools \
    xz zstd file rsync curl wget tar gzip unzip patch diffutils \
    \
    pciutils ethtool iproute2 iputils tcpdump iperf3 net-tools bridge-utils \
    nftables openssh-sftp-server chrony \
    \
    picocom tmux vim nano htop lsof strace i2c-tools can-utils libgpiod-tools \
    devmem2 usbutils mmc-utils parted e2fsprogs-resize2fs util-linux \
"

# Room to clone and build Zephyr on the board before rootfs-grow expands
# the root partition to the full eMMC/SD size on first boot.
IMAGE_ROOTFS_EXTRA_SPACE = "2097152"
