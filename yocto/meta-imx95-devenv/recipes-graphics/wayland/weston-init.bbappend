# When a display is attached, Weston opens a terminal on the imx95 tmux
# session (M7 window in front) at startup.
do_install:append() {
    printf '\n[autolaunch]\npath=%s/imx95-console-gui\n' "${bindir}" \
        >> ${D}${sysconfdir}/xdg/weston/weston.ini
}
