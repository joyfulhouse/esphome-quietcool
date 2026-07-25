# Security

This document describes the security properties of the `esphome-quietcool`
firmware so you can decide whether it fits your deployment. It is written for
someone evaluating the integration, not as an attack guide.

## The protocol has no security of its own

The QuietCool wireless system speaks a proprietary 433.92 MHz radio protocol
with **no authentication, no integrity check (no CRC), and no nonce or
freshness token**. A fan's reply is byte-for-byte identical to a command from
the OEM handheld remote — nothing in a frame identifies who sent it or proves it
came from the fan.

This is a property of the OEM protocol itself, not of this firmware. Any device
that can reproduce the protocol — the OEM remote, or any transmitter within
radio range — can both operate the fan and emit frames that look exactly like a
fan's own status report. The bridge cannot cryptographically tell those apart,
because the protocol gives it nothing to check them against.

## "Confirmed" means corroborated, not authenticated

**When this firmware reports a state as confirmed, it means the state was
corroborated by repeated, consistent observation — not that the report was
authenticated as genuinely originating at your fan.** This is the single most
important thing to understand before relying on the reported state for anything
safety-related.

A determined transmitter within RF range can, in principle, cause a fabricated
state to be reported to Home Assistant. The firmware makes that hard to do by
accident, but it cannot make it cryptographically impossible, and no firmware
running on this hardware could.

## What the firmware does protect against

The realistic, everyday risk is not a deliberate attacker — it is *incidental*
mis-attribution: a second QuietCool fan on the same band, random radio
corruption, or stale traffic being mistaken for a genuine reply. The firmware
defends against all three by accepting a candidate reply only when it satisfies
three independent bindings, each verified in the code:

- **Sender binding** — a reply is only considered if it decodes against the
  fan's own provisioned sender ID
  (`components/quietcool/core/response_classifier.cpp:24`,
  `components/quietcool/core/response_classifier.cpp:54`). Traffic from a
  different fan on the shared band is never eligible.
- **Window binding** — a reply is only accepted while the bridge is actively
  listening inside a window it opened itself, immediately after it transmitted
  a command or status query
  (`components/quietcool/core/response_classifier.cpp:53-58`). Unsolicited
  traffic arriving outside such a window cannot contribute to a confirmation.
- **Epoch binding** — each accepted reply must belong to the current command
  epoch; the guard rejects anything left over from an earlier exchange
  (`components/quietcool/core/confirmation_reducer.cpp:36-40`).

Together these mean cross-talk from a neighbouring unit, corrupted frames, and
stale replies do not silently become "confirmed" state. That is the class of
problem this firmware can and does solve.

## Why there is no stronger fix

The natural next step would be to bind an accepted reply more tightly to the
specific command that provoked it. That is already what the three bindings above
do. Beyond them there is nothing left to bind to: the bridge's internal command
token never travels over the air, and the OEM reply contains no field in which a
fan could echo one back. Adding real authentication would require a protocol the
hardware and the fan simply do not speak.

Filtering by signal strength or apparent origin is **deliberately not done**. It
cannot distinguish a transmitter sitting next to the fan from the fan itself,
and any signal threshold strong enough to matter would risk rejecting the
bridge's own fan when its signal is weak — the exact failure mode tracked as
issue #5, where the bridge must never become less able to hear its own fan.

## Residual risk, stated plainly

The limitation that remains is inherent to an unauthenticated radio protocol and
cannot be closed in firmware:

- A transmitter within RF range can influence the state this integration reports
  to Home Assistant.
- The same transmitter can already operate the fan directly, which is a strictly
  greater capability than merely corrupting a status display. Forging a
  confirmation buys an attacker nothing they could not do more simply by
  commanding the fan.
- Therefore, **if you require integrity guarantees, treat RF range as a physical
  trust boundary and control it.** Within a normal home this is not a practical
  concern; the honest statement is only that the guarantee is physical (who can
  get a radio near your house), not cryptographic.

Do not use the reported fan state as the sole input to a safety-critical
interlock where an adversary is in your threat model. See the README's Safety
section for the recommended interlock signals and their freshness caveats under
normal (non-adversarial) operation.

## Reporting a vulnerability

If you believe you have found a security issue in this project, please report it
privately rather than opening a public issue:

- Use GitHub's **private vulnerability reporting** for this repository
  (the "Report a vulnerability" button under the **Security** tab), or
- email the maintainer at the address listed on the GitHub profile that owns
  this repository.

Please include enough detail to reproduce the issue and, if you can, the
firmware version or commit you tested. We will acknowledge your report, keep you
updated on our assessment, and coordinate disclosure timing with you. Findings
that are properties of the unauthenticated OEM protocol itself — described above
— are known and documented rather than treated as new vulnerabilities.
