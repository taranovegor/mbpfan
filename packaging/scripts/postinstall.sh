#!/bin/sh
# deb: configure [old-version]; rpm: 1 = install, 2 = upgrade.
set -e
case "$1" in
	1)
		# A replaced distro mbpfan disables the unit after us; enable in %posttrans.
		mkdir -p /var/lib/rpm-state
		touch /var/lib/rpm-state/tec-mbpfan-first-install
		exit 0
		;;
	configure)
		if [ -z "$2" ]; then
			systemctl enable mbpfan.service || true
			if [ -d /run/systemd/system ]; then
				systemctl daemon-reload || true
				systemctl start mbpfan.service || true
			fi
			exit 0
		fi
		;;
esac

if [ -d /run/systemd/system ]; then
	systemctl daemon-reload || true
	systemctl try-restart mbpfan.service || true
fi
