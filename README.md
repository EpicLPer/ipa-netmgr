# ipa-netmgr

Modem and data-path manager for Qualcomm IPA v2.x modems on mainline Linux,
starting with the MSM8994 / MPSS.BO.2.5 firmware in the Lumia 950 / 950 XL
(Octagon).

It provides the userspace piece that neither ModemManager nor the vendor
daemons can provide on this platform, on **mainline kernel interfaces only**:

* **Modem lifecycle**: starts the remoteproc, detects restarts, and runs the
  firmware quirks that are not automatic (DPM ports, WDA data format, SIM
  application selection and subscription, NAS dual-standby, DMS online, radio
  cycle, network selection, SMS bearer).  This replaces the former
  `bringup.sh` / `recover.sh` helper scripts and their systemd glue.
* **DPM session**: the modem only publishes its `DATA5_CNTL` control channel
  (and the main QMI services with it) while a Data Port Mapper client holds the
  port open.  On this firmware the DPM service is only reachable over **QRTR**
  (Qualcomm's own netmgr says the same: *"they must go over IPC-router"*).
* **WDA data format**: tells the modem to use raw-ip + QMAP framing for the
  embedded data endpoint, which is what the mainline `rmnet` driver expects.
* **WDS call and rmnet link** (Linux mode): starts/stops the call on a shared
  QMI device, creates/removes the `rmnet_dataN` mux link with rtnetlink and
  applies address, default route and DNS; the link is recreated (bounded) if
  ModemManager deletes it during its modem initialization.

It is designed to serve **both** Linux and Android, which is why the
call-handling mode is configurable (`linux`: the daemon owns the call;
`android`: RIL owns the call and the daemon manages only the link).

See `notes/ipa-netmgr-spec.md` in the main repository for the full
specification and the measured platform facts.

## Status

The linux-mode flow is implemented and measured on the phone (Lumia 950 XL,
MPSS.BO.2.5): cold boot -> registered modem -> data call -> working link in
about a minute, automatic recovery after a modem restart, no helper scripts.

## ModemManager integration

ModemManager owns the QMI channel and the complete data path; the daemon is a
small platform service in front of it:

* The daemon opens every rpmsg QMI device through **qmi-proxy** (like MM), so
  several processes can share one channel (the earlier "second opener hangs up
  the endpoint" effect was two independent openers on the shared WWAN rx
  queue).
* `call_enabled=no` limits the daemon to DPM (QRTR) + UIM application
  selection/SUBSCRIPTION_OK + NAS dual-standby mask + the RAT mode preference
  (GSM/UMTS/LTE); READY is signalled right after those, before registration.
* ModemManager's qcom-soc plugin sees the QMI port (`wwan0qmi0`) and
  `rmnet_ipa0`; patch
  `patches/modemmanager/0002-port-qmi-skip-dpm-if-unsupported.patch` makes it
  skip the DPM client allocation (the service exists only over QRTR here) and
  continue with its own WDA + rmnet link management.
* NetworkManager manages the connection (IP, routes, DNS) and the
  mobile-data/radio state, so Phosh's toggles work; the link is a normal
  `rmnet` mux link created by libqmi.

Verified on the phone: cold boot -> SIM 'ready' -> MM `connected` on LTE -> NM
`wwan0qmi0` connected, 10 MB download at ~16 Mbit/s, `nmcli radio wwan off/on`
removes and recreates the connection.

The MM patch must be applied when building ModemManager (the upstream commit
used is `7088f5ea`); see the patch header for the upstream MR text.

## Building

Dependencies: glib-2.0, gobject-2.0, gio-2.0, libqmi (pkg-config `qmi-glib`),
libqrtr-glib (`qrtr-glib`), meson + ninja.

```sh
meson setup build
ninja -C build
```

## Installing

```sh
install -m755 build/src/ipa-netmgr /usr/bin/ipa-netmgr
install -m644 data/ipa-netmgr.conf.example /etc/ipa-netmgr.conf
install -m644 systemd/ipa-netmgr.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now ipa-netmgr.service
```

The unit is `Type=notify` and runs `Before=ModemManager.service`: the daemon
sends `READY=1` once the modem is registered (and the mux link exists, when
data is enabled), so ModemManager starts only when the modem is prepared.

The daemon stays in the foreground, logs to stderr (the journal when started
as a service) and releases every QMI client cleanly on SIGTERM/SIGINT.

## License

LGPL-2.1-or-later (see `COPYING`), matching the libqmi/libqrtr stack it links
against and keeping the component reusable in both Linux and Android.
