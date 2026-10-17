#!/bin/sh
systemctl enable mbpfan.service || true
if [ -d /run/systemd/system ]; then
	systemctl daemon-reload || true
	systemctl start mbpfan.service || true
fi
