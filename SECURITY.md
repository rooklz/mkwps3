# Security

MKW PS3 is a homebrew app that runs offline on your own console, so there isn't a lot of
attack surface. Two things are worth knowing:

- **The app listens on TCP port 4001** on your local network while it runs. It's a debugging
  port (logs, screenshots, remote control) with no authentication, so only run the app on a
  network you trust.
- **Only build from this repository**, and only install packages you built yourself. If
  someone's handing out ready-made `.pkg` files, that's both a legal problem and an obvious way
  to slip something nasty onto a console.

If you find something that could actually hurt someone (a way for the build to run something it
shouldn't, say, or a problem in the remote port beyond what's described above), please report
it privately through
[GitHub's private vulnerability reporting](https://github.com/rooklz/mkwps3/security/advisories/new)
rather than in a public issue. I'll get back to you as soon as I can.
