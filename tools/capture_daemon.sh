#!/bin/bash
# capture_daemon.sh — grab Genki frames on request, for scripted captures.
#
#   tools/capture_daemon.sh FLAGDIR [OUTDIR]
#
# Run it in a terminal that has macOS camera permission (the agent's own
# sandboxed shell doesn't). For each file FLAGDIR/req-NAME that appears it
# captures OUTDIR/NAME.jpg (default OUTDIR: captures/), then removes the
# request and touches FLAGDIR/done-NAME. Touch FLAGDIR/stop to end it.
flags=${1:?usage: capture_daemon.sh FLAGDIR [OUTDIR]}
out=${2:-captures}
mkdir -p "$flags" "$out"
echo "capture daemon watching $flags"
while [ ! -e "$flags/stop" ]; do
	for req in "$flags"/req-*; do
		[ -e "$req" ] || continue
		name=${req##*/req-}
		ffmpeg -hide_banner -loglevel error -f avfoundation -framerate 30 \
		    -i "ShadowCast 2" -frames:v 1 -y "$out/$name.jpg" 2>/dev/null
		rm -f "$req"
		touch "$flags/done-$name"
		echo "captured $name"
	done
	sleep 0.2
done
rm -f "$flags/stop"
echo "capture daemon stopped"
