#!/bin/sh
if [ -d /run/systemd/system ]; then
	systemctl --no-reload disable --now mbpfan.service || true
fi
