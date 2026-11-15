#!/usr/bin/env bash
# board.sh - reach the i.MX95 EVK from this development machine.
#
#   board.sh ports              list the debug-USB serial ports (FT4232H, 4 UARTs)
#   board.sh serial [a55|m7|sm] serial console; a55 auto-logs in and attaches the
#                               board tmux session (M7 + Linux windows)
#   board.sh ssh [HOST]         key-based SSH as root (attaches the board tmux session)
#   board.sh tmux               host dashboard: one tmux window per UART (+ SSH)
#
# Config (optional): ~/.config/imx95-devenv/board.conf
#   IMX95_A55_TTY=/dev/serial/by-id/...-if02-port0   # Linux console (LPUART1)
#   IMX95_M7_TTY=/dev/serial/by-id/...-if03-port0    # M7 Zephyr UART (LPUART3)
#   IMX95_SM_TTY=/dev/serial/by-id/...-if01-port0    # System Manager (LPUART2)
#   IMX95_BOARD_IP=192.168.1.50
# The default interface numbers are an UNVERIFIED guess; run "board.sh ports",
# open each one and record the real mapping in board.conf and docs/flashing.md.
#
# Host dashboard prefix is Ctrl-a, so Ctrl-b goes through to the board's tmux.
# Serial ports need the "dialout" group: sudo usermod -aG dialout $USER (re-login).
set -euo pipefail
WORK="${WORK:-$HOME/imx95-work}"
KEY="$WORK/ssh/id_ed25519"
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/imx95-devenv/board.conf"
# shellcheck disable=SC1090
[ -f "$CONF" ] && . "$CONF"

ft_port() { ls /dev/serial/by-id/*FT4232*-if0"$1"-port0 2>/dev/null | head -1 || true; }
A55="${IMX95_A55_TTY:-$(ft_port 2)}"
M7="${IMX95_M7_TTY:-$(ft_port 3)}"
SM="${IMX95_SM_TTY:-$(ft_port 1)}"
HOST="${IMX95_BOARD_IP:-}"

need_tty() {
	[ -n "$1" ] && [ -e "$1" ] || { echo "serial port not found (board connected and powered? see: $0 ports)" >&2; exit 1; }
	[ -r "$1" ] && [ -w "$1" ] || { echo "no access to $1: sudo usermod -aG dialout $USER, then log in again" >&2; exit 1; }
}
serial() { need_tty "$1"; exec picocom -q -b 115200 "$1"; }

case "${1:-}" in
ports)
	ls -l /dev/serial/by-id/ 2>/dev/null | grep -i -E 'ft4232|ftdi' || echo "no FTDI serial ports found"
	echo; echo "a55=$A55"; echo "m7=$M7"; echo "sm=$SM"
	;;
serial)
	case "${2:-a55}" in
	a55) serial "$A55" ;;
	m7)  serial "$M7" ;;
	sm)  serial "$SM" ;;
	*) echo "usage: $0 serial [a55|m7|sm]" >&2; exit 2 ;;
	esac
	;;
ssh)
	h="${2:-$HOST}"
	[ -n "$h" ] || { echo "usage: $0 ssh HOST (or IMX95_BOARD_IP in $CONF)" >&2; exit 2; }
	exec ssh -i "$KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new -t "root@$h"
	;;
tmux)
	S=imx95-host
	if ! tmux has-session -t "$S" 2>/dev/null; then
		tmux new-session -d -s "$S" -n A55 "$0 serial a55; read -rp '[A55 serial closed - Enter]'"
		tmux set-option -t "$S" prefix C-a
		tmux set-option -t "$S" status-style "bg=colour94,fg=colour255"
		tmux set-option -t "$S" status-left "#[bold] host:imx95 #[default]"
		tmux new-window -d -t "$S" -n M7-UART "$0 serial m7; read -rp '[M7 serial closed - Enter]'"
		tmux new-window -d -t "$S" -n SM-UART "$0 serial sm; read -rp '[SM serial closed - Enter]'"
		[ -n "$HOST" ] && tmux new-window -d -t "$S" -n SSH "$0 ssh; read -rp '[SSH closed - Enter]'"
	fi
	exec tmux attach -t "$S"
	;;
*)
	sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
