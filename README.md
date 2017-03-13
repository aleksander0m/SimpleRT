
# (g)SimpleRT

Reverse [Tethering](https://en.wikipedia.org/wiki/Tethering) utility for Android.

Allows you to share your computer's internet connection with your Android device via a USB cable.

For additional information, check the project page from the original SimpleRT author, **Konstantin Menyaev**, here: [https://github.com/vvviperrr/SimpleRT].

## g-simple-rt GNU/Linux command line tool

This is just a fork that reimplements the simple-rt command line tool with the following improvements:
 - Avoids any USB control transfer from within a libusb device addition callback, which triggers the "XXXX is not support accessory! Reason: Resource busy" error.
 - Runs the application in a GLib main loop, and uses GUdev to get notifications of device additions and removals.
 - Supports multiple devices doing reverse tethering in AOA mode, by applying different IP network settings to each.
 - The host network interface that the tethering will be bound to may be given with the --interface=[IFACE] CLI option.
 - The android devices to be used as AOA may be specified via --vid=[VID] or --vid=[VID] --pid=[PID]. This is so that the tool doesn't interfere with other USB devices, just with the ones we want.
 - A new --reset option allows requesting a USB reset to all AOA devices, so that they get re-enumerated.

```
$ sudo ./g-simple-rt --help
Usage:
  g-simple-rt [OPTION...] - Reverse tethering

Tethering options
  -v, --vid=[VID]             Device USB vendor ID (mandatory)
  -p, --pid=[PID]             Device USB product ID (optional)
  -i, --interface=[IFACE]     Network interface (mandatory)

Reset options
  -r, --reset                 Reset AOA devices

Application Options:
  -V, --version               Print version
  -h, --help                  Show help.
```

Dependencies:
  - libusb-1.0
  - glib-2.0
  - GUdev
  - tun/tap kernel module.

I skipped any Mac OS X support here, not personally interested in that.

Example run:
```
$ sudo ./g-simple-rt --vid=04e8 --interface=eth0
[003,006] checking AOA support...
error: AOA probing failed: Overflow
[004,120] checking AOA support...
[004,120] device supports AOA 2
device: 0x04e8:0x6865 [004:080]: tracked (candidate)
subnet mapping added: /sys/devices/pci0000:00/0000:00:1d.0/usb4/4-1/4-1.5/4-1.5.5 --> 10.11.1.0
[004,120] subnet allocated: 10.11.1.0
[004,120] sending manufacturer: The SimpleRT developers
[004,120] sending model: gSimpleRT
[004,120] sending description: Simple Reverse Tethering
[004,120] sending version: 1.0
[004,120] sending url: https://github.com/aleksander0m/SimpleRT
[004,120] sending serial: 10.11.1.2
[004,120] switching device into accessory mode...
[004,120] switch requested
uevent: remove /sys/devices/pci0000:00/0000:00:1d.0/usb4/4-1/4-1.5/4-1.5.5
device: 0x04e8:0x6865 [004:080]: untracked (candidate)
uevent: add /sys/devices/pci0000:00/0000:00:1d.0/usb4/4-1/4-1.5/4-1.5.5
device: 0x18d1:0x2d00 [004:081]: tracked (Android Open Accessory)
configuring tun0
net.ipv4.ip_forward = 1
...

$ ip addr show dev tun0
11: tun0: <POINTOPOINT,MULTICAST,NOARP,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UNKNOWN group default qlen 500
    link/none
    inet 10.11.1.1/30 scope global tun0
       valid_lft forever preferred_lft forever
    inet6 fe80::9115:8145:d6a5:e805/64 scope link flags 800
       valid_lft forever preferred_lft forever
```

## SimpleRT Android program

In order to support the g-simple-rt command line tool, the "SimpleRT" Android application also needs to be built from this repository.

Implemented as standalone service, no gui, no activities. Simplicity!

Dependencies:
  - Android 4.0 and higher.

Build system based on gradle + gradle experimental android plugin (supporting ndk). For build you need both sdk & ndk.
Create local.properties file in root dir, it should look like that:
```
ndk.dir=~/Android/Sdk/ndk-bundle
sdk.dir=~/Android/Sdk
```
build:
```
./gradlew assembleDebug
```
app/build/outputs/apk/app-debug.apk is your apk.

## License: GNU GPL v3

```
SimpleRT: Reverse tethering utility for Android

Copyright (C) 2014-2017 Gary Bisson <bisson.gary@gmail.com>
  For the linux-adk bits, see:
  https://github.com/gibsson/linux-adk.

Copyright (C) 2016-2017 Konstantin Menyaev <konstantin.menyaev@gmail.com>
  For the original SimpleRT implementation and logic, see:
  https://github.com/vvviperrr/SimpleRT

Copyright (C) 2017 Zodiac Inflight Innovations
Copyright (C) 2017 Aleksander Morgado <aleksander@aleksander.es>
  For the GLib/GUdev based port, see:
  https://github.com/aleksander0m/SimpleRT

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
```