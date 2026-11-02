#!/bin/bash
# vttest_tour.sh — drive vttest through the VeeTee and capture key screens.
#
#   tools/vttest_tour.sh FLAGDIR [LINE_PORT]
#
# Needs tools/capture_daemon.sh running on FLAGDIR (in a terminal with
# camera permission). Runs vttest inside tools/hostshell.py, steps through
# menu 1 (cursor movement), 3 (character sets) and 4 (double-sized
# characters), and asks the daemon for a capture on each screen:
# captures/vttest-<menu>-<n>.jpg. Screens advance with Return, as vttest
# prompts ("Push <RETURN>").
set -u
flags=${1:?usage: vttest_tour.sh FLAGDIR [LINE_PORT]}
port=${2:-$(ls /dev/cu.usbmodem*1 | head -1)}
cd "$(dirname "$0")"

cap() {
	touch "$flags/req-$1"
	for _ in $(seq 1 100); do
		[ -e "$flags/done-$1" ] && break
		sleep 0.1
	done
	rm -f "$flags/done-$1"
}

# A FIFO feeds keystrokes to hostshell's stdin-less session via --keys is
# awkward for long scripts, so drive the pty directly: hostshell reads
# keys from its stdin when it's not a tty only via --type/--keys, hence
# we start it with a long list of timed actions instead.
#   menu 1: 6 screens; menu 3: 3 screens; menu 4: 5 screens.
actions=(--type 'clear; TERM=vt100 vttest' --keys '')
add_menu() {
	local menu=$1 screens=$2
	actions+=(--type "$menu" --keys '' --keys '')
	for ((i = 1; i < screens; i++)); do
		actions+=(--keys '\r' --keys '' --keys '')
	done
	actions+=(--keys '\r' --keys '' --keys '')
}
add_menu 1 6
add_menu 4 5
add_menu 3 1
actions+=(--type '0' --keys '' --type 'exit')

python3 hostshell.py "$port" "${actions[@]}" --exit-after 200 \
    </dev/null >/dev/null 2>&1 &
bridge=$!

# Capture schedule mirrors the action list: 1 s per action, capture in
# the middle of each 3-action screen slot.
sleep 4.5
for menu_screens in "1 6" "4 5" "3 1"; do
	set -- $menu_screens
	for ((n = 1; n <= $2; n++)); do
		sleep 1.5
		cap "vttest-$1-$n"
		sleep 1.5
	done
	sleep 3
done
wait $bridge
echo "vttest tour done"
