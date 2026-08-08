# Disclaimer

**This software is provided free of charge, "as is", without warranty or support of any kind, and
is used entirely at your own risk.**

## Not a monitoring tool

It is a convenience indicator. It shows you, at a glance, whether ICMP echo requests to one host
have been answered recently. That is all it does.

It must **not** be relied upon where inaccuracy, delay or failure could cause loss or harm. It is
not suitable for:

- alerting, on-call rotations, or anything that pages a human
- compliance, SLA measurement, or uptime reporting
- medical, industrial, safety, financial, or life-critical systems of any kind
- being the only thing watching something that matters

Use real monitoring for real monitoring.

## What it cannot tell you

- **It only knows ICMP.** A host that answers pings can still be refusing connections, serving
  errors, or returning wrong data. A host that ignores pings may be perfectly healthy — plenty of
  networks and firewalls drop ICMP by policy.
- **Latency is approximate.** Round trip times come from the operating system's ICMP stack and are
  affected by scheduling, power management, and whatever else the machine is doing.
- **It measures one path.** A result says something about the route between this machine and that
  host at that moment. It says nothing about anyone else's experience.

## Undocumented Windows behaviour

To sit inside the taskbar, this app re-parents its window into the shell's own window
(`Shell_TrayWnd`). **This is not a supported or documented Windows API.** Microsoft may change or
remove the behaviour it depends on at any time, without notice, in any update.

The app falls back to a floating always-on-top window when this fails, but that fallback is itself
best-effort. If a Windows update breaks it entirely, that is an expected outcome of relying on
undocumented behaviour, not a defect that anyone is obliged to fix.

## Liability

To the maximum extent permitted by applicable law, the author accepts **no liability of any kind**
arising from the use of this software, including but not limited to damage to any machine, system,
network or data; lost time; lost revenue; or any direct, indirect, incidental, special, exemplary
or consequential damages, however caused and on any theory of liability.

By using this software you accept that you do so entirely at your own risk, and that you are
responsible for evaluating its suitability for whatever you intend to use it for.

Full terms: [MIT License](LICENSE).
