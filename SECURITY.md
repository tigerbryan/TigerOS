# Security Policy

TigerOS is currently an early open-source foundation for ESP32-S3 devices. Treat the default configuration as a development baseline and harden it before exposing a device or cloud service to untrusted networks.

## Important Defaults

- Default Web Console username: `admin`
- Default Web Console password: `tigeros`
- Change device passwords and API tokens before production use.
- Do not expose the local Web Console directly to the public internet.
- Keep webhook secrets, device tokens, JWT secrets, and Supabase/Postgres credentials outside git.

## Reporting Issues

Please report security issues privately to the repository owner first if possible. If the issue is not sensitive, open a GitHub issue with enough detail to reproduce it.

## Supported Scope

The open-source project includes firmware and a cloud foundation. Production deployments are responsible for:

- Network isolation
- HTTPS certificates
- Secret rotation
- Database backups
- Device ownership and access policy
- Physical device security
