#!/usr/bin/env bash
# Fetch the NXP i.MX Yocto BSP and create the imx95-devenv build directory.
# Runs INSIDE the build container:  scripts/container.sh $REPO/scripts/yocto-setup.sh
#
# Env overrides: BSP_MANIFEST, BSP_BRANCH, MACHINE, DISTRO, IMX_SOC_REV
# Requires ACCEPT_FSL_EULA=1 in the environment: building the NXP BSP means
# accepting the NXP EULA (sources/meta-imx/EULA.txt).
set -euo pipefail
: "${WORK:?run via scripts/container.sh}" "${REPO:?run via scripts/container.sh}"

BSP_BRANCH="${BSP_BRANCH:-imx-linux-wrynose}"
BSP_MANIFEST="${BSP_MANIFEST:-imx-6.18.37-2.1.0.xml}"
MACHINE="${MACHINE:-imx95-19x19-lpddr5-evk}"
DISTRO="${DISTRO:-fsl-imx-wayland}"
IMX_SOC_REV="${IMX_SOC_REV:-B0}"
Y="$WORK/yocto"

if [ "${ACCEPT_FSL_EULA:-0}" != 1 ]; then
    echo "Set ACCEPT_FSL_EULA=1 to confirm you accept the NXP EULA (sources/meta-imx/EULA.txt)." >&2
    exit 1
fi

mkdir -p "$Y" && cd "$Y"
if [ ! -d .repo ]; then
    repo init -q -u https://github.com/nxp-imx/imx-manifest -b "$BSP_BRANCH" -m "$BSP_MANIFEST" </dev/null
fi
repo sync -q -j8

# NXP setup script (creates build/, accepts EULA via EULA=1, adds NXP layers)
set +u
EULA=1 DISTRO="$DISTRO" MACHINE="$MACHINE" source ./imx-setup-release.sh -b build >/dev/null
set -u

bitbake-layers add-layer "$REPO/yocto/meta-imx95-devenv"

marker="# ---- imx95-devenv ----"
if ! grep -q "$marker" conf/local.conf; then
    sed "s|@WORK@|$WORK|g; s|@IMX_SOC_REV@|$IMX_SOC_REV|g" \
        "$REPO/yocto/local.conf.append" >> conf/local.conf
fi
echo "Build dir ready: $Y/build  (MACHINE=$MACHINE DISTRO=$DISTRO IMX_SOC_REV=$IMX_SOC_REV)"
