# mbpfan

[![Build](https://github.com/taranovegor/mbpfan/actions/workflows/build.yml/badge.svg)](https://github.com/taranovegor/mbpfan/actions/workflows/build.yml)

This is an enhanced version of [Allan McRae mbpfan](http://allanmcrae.com/2010/05/simple-macbook-pro-fan-daemon/)

mbpfan is a daemon that uses input from coretemp module and sets the fan speed using the applesmc module.
This enhanced version assumes any number of processors and fans (max. 10).

* It only uses the temperatures from the processors as input.
* It requires coretemp and applesmc kernel modules to be loaded.
* It requires root use
* It daemonizes or stays in foreground
* Verbose mode for both syslog and stdout
* Users can configure it using the file /etc/mbpfan.conf

**Table Of Contents**

- [Fork Differences from Upstream](#fork-differences-from-upstream)
- [Supported GNU/Linux Distributions](#supported-gnulinux-distributions)
- [Tested Mac Models](#tested-mac-models)
- [Requirements](#requirements)
- [Installation](#installation)
	- [Debian, Ubuntu, Fedora, Arch Linux](#debian-ubuntu-fedora-arch-linux)
	- [Gentoo](#gentoo)
	- [Solus](#solus)
	- [Generic Install Instructions (All Other Operating Systems)](#generic-install-instructions-all-other-operating-systems)
- [Run Instructions](#run-instructions)
- [Starting at boot](#starting-at-boot)
- [Usage](#usage)
- [References](#references)
- [License](#license)
- [Credits](#credits)

## Fork Differences from Upstream

This fork replaces the `low_temp`/`high_temp`/`max_temp` linear ramp with a different control algorithm:

* EMA-smoothed temperature reading (avoids fan hunting on short load bursts)
* multi-point `curve` config option (`temp_c:percent` pairs, linearly interpolated) instead of three fixed thresholds
* asymmetric slew rate limiting (`up_rate`/`down_rate`) so the fan ramps toward the target instead of snapping to it
* emergency slam to 100% if the raw (unsmoothed) temperature stays at or above `hard_max_temp` for `hard_max_hold` consecutive polls
* re-asserts manual fan control every poll (survives suspend/resume dropping it back to auto)

New `/etc/mbpfan.conf` keys: `up_rate`, `down_rate`, `temp_alpha_percent`, `hard_max_temp`, `hard_max_hold`, `curve`. The `low_temp`/`high_temp`/`max_temp` keys are gone - see [mbpfan.conf](mbpfan.conf) for a documented example.

## Supported GNU/Linux Distributions

We provide scripts to to load mbpfan daemon at system boot for many distros.
Please note that the support is provided by volunteers. mbpfan needs tests and bug reports.

Supported distributions:

- Ubuntu
- Debian
- Archlinux
- Fedora
- RedHat
- CentOS
- Gentoo
- Alpine
- Trisquel
- Solus

## Tested Mac Models

See https://github.com/linux-on-mac/mbpfan/wiki/Tested-Mac-Models .

## Requirements

Be sure to load the kernel modules **applesmc** and **coretemp**.

These modules are often automatically loaded when booting up GNU/Linux on a MacBook. If that is not the case, you should make sure to load them at system startup.

**How do I know if applesmc and coretemp are loaded?**

In most distributions, you can run the following command:

```bash
lsmod | grep -e applesmc -e coretemp
```

If you see `coretemp` and `applesmc` listed, you are all set.

**If you do not see `coretemp` and `applesmc` listed, you must load them.**

This is _usually_ achieved by inserting the following two lines in the file `/etc/modules`

```
coretemp
applesmc
```

Please check the relevant documentation of your GNU/Linux distribution.

## Installation

### Debian, Ubuntu, Fedora, Arch Linux

Download the package for your system from the
[latest release](https://github.com/taranovegor/mbpfan/releases/latest) and install it:

```
# Debian, Ubuntu
sudo apt install ./tec-mbpfan_*_amd64.deb

# Fedora
sudo dnf install ./tec-mbpfan-*.x86_64.rpm

# Arch Linux
sudo pacman -U tec-mbpfan-*-x86_64.pkg.tar.zst
```

The package enables and starts the mbpfan service on first install.
If you turn the service off later, upgrades will not turn it back on.

The package is called `tec-mbpfan`. It replaces the `mbpfan` package from your
distribution, so a system update will not bring the upstream version back.
The program, the service and the config file are still called `mbpfan`.

If you had upstream mbpfan before and changed `/etc/mbpfan.conf`, your old file
stays in place. This fork uses a different config format, so check the file
against [mbpfan.conf](mbpfan.conf).

The packages need glibc 2.34 or newer (Debian 12, Ubuntu 22.04, Fedora 35 or newer).
On older systems, use the Generic Install Instructions below.

### Gentoo

Install the ```mbpfan``` package with:

    sudo emerge -av app-laptop/mbpfan


### Solus

On Solus, install the package with:
```
sudo eopkg install mbpfan
```
then enable the service.


### Generic Install Instructions (All Other Operating Systems)

Compile with

    make

Install with

    sudo make install

It copies mbpfan to /usr/sbin, mbpfan.conf to /etc (and overwrites existing files),
README.md to /usr/share/doc/mbpfan, and mbpfan.8.gz to /usr/share/man/man8

If you would like to compile with Clang instead of GCC, simply set your system's
default compiler to be Clang. Tested with Clang 3.8 and 3.9. Tested with Clang
4.0 along with llvm-lld (The LLVM Linker).


Run The Tests (Optional)
------------------------
Users may run the tests after building the program.  Please run the following command _from within the source directory_.

    sudo make tests

Note that this only works on MacBook and not desktop computers due to different environment expectations.


## Run Instructions

If not installed, run with

    sudo bin/mbpfan

If installed, manually run with

    sudo mbpfan

If installed and using the init file, run with (Ubuntu example)

    sudo service mbpfan start


## Starting at boot

**Ubuntu**

For systemd based init systems (Ubuntu 16.04+), see the systemd section below.

For upstart based init systems (Ubuntu before 16.04), an example upstart job has been provided.
For using it, execute:

    sudo cp mbpfan.upstart /etc/init/mbpfan.conf
    sudo start mbpfan

**Debian**
An init file suitable for /lib/lsb/init-functions (Debian)
is located in the main folder of the source files, called mbpfan.init.debian
Rename it to mbpfan, give it execution permissions (chmod +x mbpfan)
and move it to /etc/init.d
Then, add it to the default runlevels with (as root):

    sudo update-rc.d mbpfan defaults

**Redhat, CentOS, Fedora**
An init file suitable for /etc/rc.d/init.d/functions
(RHEL/CentOS & Fedora) is also located at the same place, this file is called
mbpfan.init.redhat. Also rename it to mbpfan, give it execution permissions
and move it to /etc/init.d
To add the script to the default runlevels, run the following as root:

    chkconfig --level 2345 mbpfan on && chkconfig --level 016 mbpfan off

**Gentoo**

To automatically run mbpfan at boot, run as root:

    rc-update add mbpfan default


**systemd**
The packages enable and start the service on first install.
After `make install`, run this to start mbpfan now and at every boot:

    sudo systemctl daemon-reload
    sudo systemctl enable --now mbpfan.service

**dinit**
A [dinit](https://github.com/davmac314/dinit) service definition is also included. To use it, execute the following:
    
    sudo cp mbpfan.dinit /etc/dinit.d/mbpfan
    sudo dinitctl enable mbpfan
    sudo dinitctl start mbpfan

## Usage

    Usage: ./mbpfan OPTION(S)

    -h Show the help screen
    -f Run in foreground
    -v Be (a lot) verbose

## References

* [mbpfan on Ask Ubuntu](https://askubuntu.com/search?q=mbpfan+is%3Aquestion)

## License

GNU General Public License version 3

## Credits

**This Project Is Based On:**

* http://allanmcrae.com/2010/05/simple-macbook-pro-fan-daemon/
* http://allanmcrae.com/2011/08/mbp-fan-daemon-update/
* https://launchpad.net/macfanctld
* https://www.lobotomo.com/products/FanControl/

**This Project uses following library:**

* [ANSI C Application Settings Management](http://pokristensson.com/settings.html) by Per Ola Kristensson.
