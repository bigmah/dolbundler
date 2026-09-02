#!/bin/sh
# Stops a running driver, whether it came from start.sh or was launched directly.
#
# Sends SIGINT, which the driver installs a handler for: it leaves the read loop,
# closes the pipe cleanly and prints the session summary. That is exactly what
# Ctrl-C in start.sh does, so this is the same shutdown by remote control rather
# than a different one. Escalation to SIGTERM (also handled) and then a hint
# about SIGKILL only happens if it stops responding.
#
# Dolphin is deliberately left alone. Closing an emulator out from under a
# running game would lose progress, and that is not a stop script's call to make.
set -e

NAME=gc_controller
# How long to wait for a clean exit, in 0.2s steps.
STEPS=10

pids() { pgrep -x "$NAME" 2>/dev/null || true; }

RUNNING=$(pids)
if [ -z "$RUNNING" ]; then
	echo "no $NAME running"
	exit 0
fi

echo "stopping $NAME (pid $(echo "$RUNNING" | tr '\n' ' '))"
# Word splitting is wanted: there may be more than one.
# shellcheck disable=SC2086
kill -INT $RUNNING 2>/dev/null || true

i=0
while [ "$i" -lt "$STEPS" ]; do
	sleep 0.2
	RUNNING=$(pids)
	if [ -z "$RUNNING" ]; then
		echo "stopped."
		exit 0
	fi
	i=$((i + 1))
done

echo "still running after 2s — sending SIGTERM"
# shellcheck disable=SC2086
kill -TERM $RUNNING 2>/dev/null || true
sleep 0.5

RUNNING=$(pids)
if [ -n "$RUNNING" ]; then
	echo "did not exit; force it with: kill -9 $(echo "$RUNNING" | tr '\n' ' ')" >&2
	exit 1
fi
echo "stopped."
