# /etc/profile.d/zz-imx95-console.sh
# Interactive logins on the serial console or over SSH attach to the imx95
# tmux session (M7 + Linux windows). Detach (Ctrl-b d) to get a plain shell.
# Opt out: touch ~/.no-imx95-console, or log in with IMX95_CONSOLE=0.
case $- in *i*) ;; *) return 0 ;; esac
[ -z "${TMUX:-}" ] || return 0
[ "${IMX95_CONSOLE:-1}" = 1 ] || return 0
[ ! -e "$HOME/.no-imx95-console" ] || return 0
command -v imx95-console >/dev/null 2>&1 || return 0
case "$(tty 2>/dev/null)" in
	/dev/ttyLP*) imx95-console attach ;;
	/dev/pts/*) [ -n "${SSH_TTY:-}" ] && imx95-console attach ;;
esac
true
