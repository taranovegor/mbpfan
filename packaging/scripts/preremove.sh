#!/bin/sh
# deb: remove|upgrade; rpm: 0 = erase, 1 = upgrade.
set -e
if [ "$1" = remove ] || [ "$1" = 0 ]; then
	if [ -d /run/systemd/system ]; then
		systemctl --no-reload disable --now mbpfan.service || true
	fi
fi
