# ADR 0001: The tray trusts only a compositor-owned `org.wayvibrance`

- Status: Accepted
- Date: 2026-11-12
- Deciders: Way Vibrance maintainers

## Context

The tray controls the KWin effect over D-Bus. The well-known service name
`org.wayvibrance` is not owned by the compositor itself; any process running
as the same session user can request (and win) that name before the effect
registers it. A hostile peer that owns the name could therefore serve fake
state, reject legitimate requests, or even answer with values that make the
UI misrepresent the effect.

The tray must therefore verify that the current owner of `org.wayvibrance`
is the compositor process, not merely that the name is present.

## Decision

Trust the owner only when its **PID** equals the PID of the owner of
`org.kde.KWin`:

1. `serviceOwnerOf("org.wayvibrance")` returns the unique connection name
   (e.g. `:1.42`).
2. `pidOfBusName(owner)` and `pidOfBusName("org.kde.KWin")` resolve PIDs via
   the D-Bus daemon's `GetConnectionUnixProcessID`.
3. The owner is trusted only if both PIDs are non-zero and equal.

Hardening rules, deliberately not relaxed:

- **No `/proc/<pid>/comm` fallback.** `prctl(PR_SET_NAME)` lets a process
  forge its command name, so process names are not an identity anchor.
- **`org.kde.kwin.Effects` is not accepted.** Every KWin 6 release owns
  `org.kde.KWin` unconditionally, while the legacy name is usually free.
  Accepting it would let a malicious peer own both names and pass the PID
  comparison.
- **Empty or unknown owners fail closed.** No owner, or a name the bus
  cannot resolve, is treated as untrusted.
- **A direct owner swap does not inherit trust.** The tray re-resolves and
  re-verifies the new owner on every registration/unregistration instead of
  caching trust across handovers.

Implementation lives in `tray/dbus_security_p.h` and is covered by
`tests/dbus_security_test.cpp`.

## Consequences

- A peer that grabs `org.wayvibrance` before KWin cannot pass verification,
  because its PID will differ from `org.kde.KWin`'s (or `org.kde.KWin` will
  be unowned and the check fails closed).
- The tray needs a working session bus and a compositor owning
  `org.kde.KWin`; in headless/test environments the check always fails
  closed, which is the safe default.
- On every owner change the tray pays a few synchronous, time-bounded D-Bus
  identity calls. These are intentionally bounded so a wedged bus cannot
  freeze the tray.
