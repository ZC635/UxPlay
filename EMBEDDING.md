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
