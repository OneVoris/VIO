# Security Policy

## Reporting

Report vulnerabilities through the hosting platform's private security-reporting mechanism. Do not open a public issue for an unpatched vulnerability.

Include affected versions, configuration, platform, impact, a minimal reproducer, and any suspected cross-repository dependency.

## Supported Versions

Until the first stable release, only the newest development line receives security fixes. A supported-version matrix will be added before `1.0`.

## Security Boundaries

- Use-after-free in cancellation and native-handle reuse.
- Unbounded queues or task creation.
- Blocking the shard thread.
- Double completion and stale events.

## Expectations

- Peer-controlled sizes, counts, nesting, queues, and work have hard limits.
- Secure defaults cannot be disabled implicitly by missing configuration.
- Secrets and untrusted payloads are not written to normal logs.
- Third-party security updates are tracked at provider boundaries.
- Security fixes include regression tests and coordinated upstream/downstream releases when required.
