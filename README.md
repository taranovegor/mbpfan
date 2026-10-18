# mbpfan

[![Build](https://github.com/taranovegor/mbpfan/actions/workflows/build.yml/badge.svg)](https://github.com/taranovegor/mbpfan/actions/workflows/build.yml)

A fan control daemon for MacBooks running Linux. It reads CPU temperatures
from the `coretemp` driver and sets fan speeds through the `applesmc` driver.

This is a fork of [mbpfan](https://github.com/linux-on-mac/mbpfan). The goal
is a fan that stays quiet and changes speed slowly, instead of sitting silent
until the laptop gets hot and then running at full speed for a long time.
The fork has its own releases and packages, and its config format is not
compatible with upstream.

- [How it differs from upstream](#how-it-differs-from-upstream)
- [Requirements](#requirements)
- [Installation](#installation)
- [Check that it works](#check-that-it-works)
- [Configuration](#configuration)
- [Tuning the curve](#tuning-the-curve)
- [Command line](#command-line)
- [Building from source](#building-from-source)
- [License](#license)
- [Credits](#credits)

## How it differs from upstream

**Smoothed temperature.** Each new reading is blended into a smoothed
average (an exponential moving average). With the default settings the
average lags the real temperature by about ten readings, which is ten
seconds at one reading per second. Short load spikes, like opening a
browser tab, barely move the fan.

**A fan curve instead of three thresholds.** Upstream maps temperature to
speed with a straight line between `low_temp` and `high_temp`. Here you set
a list of `temperature:percent` points, and speeds between them are
interpolated. You decide how flat or steep each part of the curve is.

**Gradual speed changes.** The fan moves toward its target speed at a set
number of RPM per second. The rates for speeding up and slowing down are
separate (`up_rate`, `down_rate`), so by default it spins up slowly and
calms down faster.

**Emergency limit.** If the raw temperature (not the smoothed one) stays at
or above `hard_max_temp` for `hard_max_hold` readings in a row, the fan goes
straight to maximum speed. Smoothing never delays a real overheat.

**Survives suspend.** After resume, the firmware can quietly take fan
control back. mbpfan sets manual mode and the current speed again on every
reading, so it takes over again right away.

**Kernel 7.3 support.** Linux 7.3 moved the applesmc fan files under
`hwmon` and renamed them (`fanN_target` and `pwmN_enable` instead of
`fanN_output` and `fanN_manual`). mbpfan detects which layout the running
kernel uses and works with both, from older kernels up to 7.3.

## Requirements

- A MacBook or MacBook Pro with an Intel CPU.
- The `coretemp` and `applesmc` kernel modules.
- 64-bit Linux (x86_64, which the release files call `amd64`) with glibc
  2.34 or newer. That is Debian 12, Ubuntu 22.04, Fedora 35 or any later
  release, and any Arch Linux updated since 2021. To check another system,
  run `ldd --version`.
- systemd, if you want the service to start at boot.

Most distributions load both modules automatically on a Mac. To check:

```
lsmod | grep -e applesmc -e coretemp
```

If one is missing, load both now:

```
sudo modprobe -a coretemp applesmc
```

To load them at every boot, create `/etc/modules-load.d/mbpfan.conf` with
one module name per line:

```
coretemp
applesmc
```

The packages install `/usr/lib/modules-load.d/mbpfan.depend.conf`, which
loads `coretemp` only. `applesmc` is usually loaded by the system on its
own.

People have reported which Mac models work in the
[upstream wiki](https://github.com/linux-on-mac/mbpfan/wiki/Tested-Mac-Models).
The hardware side has not changed in this fork, so that list still applies.

## Installation

Download the file for your system from the
[releases page](https://github.com/taranovegor/mbpfan/releases/latest), then
run the commands below from the folder where you saved it. Each release has
packages for Debian/Ubuntu, Fedora and Arch Linux, plus a plain binary for
other systems.

To check a package download, also save `SHA256SUMS` and run:

```
sha256sum -c --ignore-missing SHA256SUMS
```

### Debian, Ubuntu

```
sudo apt install ./tec-mbpfan_*_amd64.deb
```

### Fedora

```
sudo dnf install ./tec-mbpfan-*.x86_64.rpm
```

### Arch Linux

```
sudo pacman -U tec-mbpfan-*-x86_64.pkg.tar.zst
```

### About the packages

The package is called `tec-mbpfan`, but the program, the service and the
config file are still called `mbpfan`.

If upstream mbpfan is installed, you don't need to remove it first. The
package replaces it, and a system update will not bring it back.

On first install the package enables and starts `mbpfan.service`. If you
disable the service later, upgrades leave it disabled.

`/etc/mbpfan.conf` is kept when you install or upgrade. If you edited it
for upstream mbpfan, it stays as it was. Its `low_temp`, `high_temp` and
`max_temp` lines are ignored and the default curve is used, so the fans
still work. `min_fanN_speed` and `max_fanN_speed` lines are still read. To
use the new settings, compare the file with [mbpfan.conf](mbpfan.conf).

To remove the package, use `apt remove tec-mbpfan`, `dnf remove tec-mbpfan`
or `pacman -R tec-mbpfan`. When the service stops normally, it hands the
fans back to automatic control. If mbpfan is killed or crashes, the fans
stay at their last speed until it starts again.

### Plain binary

`mbpfan-linux-amd64` is the same program without a package. Download it
together with `mbpfan-linux-amd64.sha256`, then:

```
sha256sum -c mbpfan-linux-amd64.sha256
sudo install -m 0755 mbpfan-linux-amd64 /usr/sbin/mbpfan
```

The binary runs without a config file and uses the built-in defaults. To
change them, save [mbpfan.conf](mbpfan.conf) as `/etc/mbpfan.conf`. To run
it at boot with systemd, save [mbpfan.service](mbpfan.service) to
`/etc/systemd/system/` and run:

```
sudo systemctl daemon-reload
sudo systemctl enable --now mbpfan.service
```

On a system without systemd, have your init system run `/usr/sbin/mbpfan`
as root. Without `-f` it forks into the background. With `-f` it stays in
the foreground, which suits supervisors such as runit or s6. Either way it
writes its PID to `/run/mbpfan.pid`.

To remove the binary:

```
sudo systemctl disable --now mbpfan.service
sudo rm /usr/sbin/mbpfan /etc/systemd/system/mbpfan.service /etc/mbpfan.conf
sudo systemctl daemon-reload
```

### From source

You need a C compiler and `make`. Build from the `master` branch:

```
git clone -b master https://github.com/taranovegor/mbpfan.git
cd mbpfan
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now mbpfan.service
```

Run `make` as your own user first, so `sudo make install` only copies
files. It installs the program to `/usr/sbin`, the service to
`/lib/systemd/system`, the config to `/etc` and the man page.

`make install` overwrites `/etc/mbpfan.conf`, so back it up before you
reinstall or upgrade this way. `sudo make uninstall` removes everything it
installed, the config file included. Stop the service before that with
`sudo systemctl disable --now mbpfan.service`.

## Check that it works

```
systemctl status mbpfan
journalctl -u mbpfan -f
```

The service should be `active (running)`. To watch the real fan speed in
RPM, read `fan1_input`. Its location depends on the kernel:

```
# kernel older than 7.3
cat /sys/devices/platform/applesmc.768/fan1_input
# kernel 7.3 and newer
cat /sys/devices/platform/applesmc.768/hwmon/hwmon*/fan1_input
```

## Configuration

The config file is `/etc/mbpfan.conf`. The copy in this repository,
[mbpfan.conf](mbpfan.conf), has a comment for every setting. The main ones:

| Key | Default | Meaning |
|---|---|---|
| `curve` | `65:0,75:15,82:35,88:65,95:100` | `temperature:percent` points, sorted by temperature, 2 to 16 points |
| `temp_alpha_percent` | `10` | Weight of a new reading in the average, 1 to 100. Lower is smoother and slower. |
| `up_rate` | `350` | Max RPM increase per second |
| `down_rate` | `600` | Max RPM decrease per second |
| `hard_max_temp` | `103` | Raw temperature (°C) that counts toward the emergency limit |
| `hard_max_hold` | `3` | Readings in a row at that temperature before full speed |
| `polling_interval` | `1` | Seconds between readings |
| `min_fanN_speed`, `max_fanN_speed` | from applesmc | Override the speed range of fan N (numbered from 1, as in `fanN_min`) |

Percent in `curve` is a share of the fan's own range: 0% is its hardware
minimum (`fanN_min`), not a stopped fan, and 100% is its maximum. Below the
first point the fan stays at that point's speed, and above the last point
it stays at the last one.

The values are not range-checked, so keep percentages between 0 and 100.
A setting of `0` is ignored and the previous value stays. If `curve` has a
mistake in it (a bad point, fewer than two points, or temperatures out of
order), mbpfan logs an error and keeps the curve it had.

After editing the file, apply it without a restart:

```
sudo systemctl reload mbpfan
```

A reload reads every key again. If you delete a line, the old value stays
until the next restart. New fans and sensors are only found on restart.

## Tuning the curve

For a quiet idle, find the highest fan speed you can't hear, or can only
just hear, and make it the first point of the curve. A slightly faster fan
at idle keeps the Mac cooler, so short loads don't push the speed up.

The curve works in percent, so first find the range of your fan:

```
# kernel older than 7.3
cat /sys/devices/platform/applesmc.768/fan1_{min,max}
# kernel 7.3 and newer
cat /sys/devices/platform/applesmc.768/hwmon/hwmon*/fan1_{min,max}
```

To turn RPM into percent:

```
(rpm - fan_min) * 100 / (fan_max - fan_min)
```

To try a fixed speed, set a flat curve, for example `curve = 0:20,100:20`,
reload, and listen. Change the number until you find the speed you want,
then build the real curve on top of it, raising the other points if they
are lower.

To see what mbpfan does on each reading, run it by hand in verbose mode.
Only one copy can run at a time, so stop the service first:

```
sudo systemctl stop mbpfan
sudo mbpfan -f -v
# Ctrl+C when done
sudo systemctl start mbpfan
```

Each reading then prints the raw and smoothed temperature, the curve
target and the speed set for each fan.

## Command line

```
Usage: mbpfan OPTION(S)
Options:
	-h Show this help screen
	-f Run in foreground
	-v Be (a lot) verbose
```

mbpfan needs root. Under systemd it logs to the journal.

## Building from source

To install from source, see [From source](#from-source). This section is
about working on the code.

```
make
```

This builds `bin/mbpfan` and the test program `bin/mbpfan-tests`. The tests
use the real hardware, so they only run on a Mac with `coretemp` and
`applesmc` loaded. Run them as root from the top of the repository, with
`/etc/mbpfan.conf` in this fork's format. One test puts the CPU under load
on purpose.

```
sudo ./bin/mbpfan-tests
```

CI builds every push but does not run the tests.

## License

GNU General Public License version 3. See [COPYING](COPYING).

## Credits

This fork is maintained by Egor Taranov. It is built on
[mbpfan](https://github.com/linux-on-mac/mbpfan) and the work of everyone
listed in [AUTHORS](AUTHORS).

mbpfan itself is based on:

- [Simple MacBook Pro fan daemon](http://allanmcrae.com/2010/05/simple-macbook-pro-fan-daemon/) and its [update](http://allanmcrae.com/2011/08/mbp-fan-daemon-update/) by Allan McRae
- [macfanctld](https://launchpad.net/macfanctld)
- [FanControl](https://www.lobotomo.com/products/FanControl/)

Config parsing uses
[ANSI C Application Settings Management](http://pokristensson.com/settings.html)
by Per Ola Kristensson.
