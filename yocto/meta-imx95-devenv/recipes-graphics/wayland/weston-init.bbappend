# When a display is attached, Weston opens a terminal on the imx95 tmux
# session (M7 window in front) at startup.
#
# require-outputs=none: keep Weston running with no monitor connected and pick
# the HDMI output up on hotplug. Without it Weston exits ("Could not enable any
# output") when HDMI-A-1 is disconnected at boot, systemd gives up after five
# restarts, and a monitor plugged in later stays dark (seen on hardware
# 2026-11-16).
do_install:append() {
    sed -i 's/^\[core\]$/[core]\nrequire-outputs=none/' ${D}${sysconfdir}/xdg/weston/weston.ini
    grep -q '^require-outputs=none' ${D}${sysconfdir}/xdg/weston/weston.ini || \
        printf '\n[core]\nrequire-outputs=none\n' >> ${D}${sysconfdir}/xdg/weston/weston.ini
    printf '\n[autolaunch]\npath=%s/imx95-console-gui\n' "${bindir}" \
        >> ${D}${sysconfdir}/xdg/weston/weston.ini
}
