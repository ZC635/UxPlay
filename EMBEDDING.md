# Embedding this UxPlay fork

This fork retains the upstream command-line receiver and adds the APIs used by
ZC635/AirPlay-Receiver: recoverable renderer initialization, decoded video appsink
access, aspect-ratio/window control, and optional recording sample callbacks.

When a host already publishes AirPlay services (for example with QMdnsEngine), set
`UXPLAY_EXTERNAL_MDNS_PUBLISHER=ON` before adding UxPlay as a subdirectory. In this
mode DNS-SD registration builds TXT records for the host and never opens a second
mDNS publisher. Do not combine it with `USE_DNS_SD`. The default is OFF, so the
standalone `uxplay` executable keeps upstream's internal mDNS implementation.
`dnssd_init` follows the upstream argument order: `pin_pw`, then `error`.

Build with `BUILD_TESTING=ON` and run CTest for the renderer and protocol regressions.
AirPlay-Receiver also carries integration tests for library consumption, multicast
ownership, mirror re-key, audio sequence continuity, and fragmented HTTP clients.
The parent receiver's full tests validate discovery, playback/recording lifecycle,
portable runtime dependencies, and startup behavior; actual iPhone/iPad playback
and long-running A/V synchronization still require hardware acceptance.

## October 2026 memory backport

This fork selectively backports FDH2/UxPlay `60e6430`, `517cbb1`, and
`b29411f` onto `d347e6e`, rather than merging the upstream renderer and protocol
refactors. The existing SRP proof/public-key bounds checks were already present.
The backport adds EPK/authTag and SETUP EIV/EKEY length checks, releases SRP
salt/verifier/public-key and OpenSSL temporaries, and cleans up pending mirror
buffers and both RTP timestamp sessions. `srp_new_user` returns a borrowed salt
and a caller-owned public key; callers must `free` the latter.

The mirror implementation retains dynamic SPS/PPS buffers and its existing
callback ABI; only exit cleanup is backported from `517cbb1`. RTP retains both
control and data timestamp sessions (upstream had removed the control session),
so stop releases both, clears the pointers, and joins a worker even if it already
cleared its running flag. Renderer transitions, sample taps, external mDNS,
reverse-HTTP parsing, and X11 handling are unchanged.

Five regressions cover handler bounds, SETUP bounds, repeated SRP/base64 allocation
balance, RTP stop/restart, and mirror EOF with pending SPS/PPS plus a partial
payload. `PairingMemoryTest` requires GNU ld wrapping and tracks both static
library allocations and OpenSSL allocations; it is enabled for GCC on non-Apple
platforms. Handler boundary tests replace only proof/confirmation/FairPlay crypto calls;
the existing pairing tests perform real SRP verification. These tests do not
constitute an end-to-end Apple-device pairing or playback test.
