#!/bin/sh
marker=/var/lib/rpm-state/tec-mbpfan-first-install
[ -e "$marker" ] || exit 0
rm -f "$marker"
systemctl enable mbpfan.service || true
if [ -d /run/systemd/system ]; then
	systemctl daemon-reload || true
	systemctl start mbpfan.service || true
fi
