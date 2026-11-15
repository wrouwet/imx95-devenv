# imx95-devenv U-Boot changes (default environment only):
#  - fdtfile defaults to the devenv dtb (CONFIG_DEFAULT_FDT_FILE)
#  - before loading the dtb, "devenv_pickfdt" chooses the variant for the
#    MIPI-DSI->HDMI adapter: LT9611UXC if it answers on i2c 1 @0x2b, else
#    ADV7535 (NXP's EVK default). Override at the U-Boot prompt with
#      setenv devenv_display adv7535|lt9611uxc|none|auto; saveenv
# A board with a saved environment from an older image ignores all of this;
# reset it once with:  env default -a; saveenv
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append:mx95-nxp-bsp = " file://devenv-fdt.cfg file://devenv-env.txt"

do_configure:prepend:mx95-nxp-bsp() {
    envf=${S}/board/nxp/imx95_evk/imx95_evk.env
    if [ -f "$envf" ] && ! grep -q devenv_pickfdt "$envf"; then
        awk -v add="${UNPACKDIR}/devenv-env.txt" '
            /^loadfdt=/ {
                while ((getline l < add) > 0) print l
                sub(/^loadfdt=/, "loadfdt=run devenv_pickfdt; ")
            }
            { print }' "$envf" > "$envf.new" && mv "$envf.new" "$envf"
        grep -q '^loadfdt=run devenv_pickfdt' "$envf" || bbfatal "devenv: failed to patch $envf"
    fi
}
