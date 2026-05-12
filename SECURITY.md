# Security Policy

## Supported Versions

Currently, only the `main` branch is receiving security updates.

| Version | Supported          |
| ------- | ------------------ |
| main    | :white_check_mark: |
| 1.0     | :x:                |

## Reporting a Vulnerability

Because this engine acts as financial technology infrastructure, security is paramount. Memory corruption, integer overflows, or logical zero-days could lead to severe financial consequences in a production environment.

**Do NOT report security vulnerabilities via public GitHub issues.**

If you discover a vulnerability:
1. Please email the maintainer privately at the email address listed on their GitHub profile, or reach out securely.
2. Include steps to reproduce the vulnerability.
3. Include the environment details (Compiler, OS, Architecture).
4. If applicable, attach an ASAN/UBSAN log demonstrating the memory corruption or overflow.

You can expect an initial acknowledgment within 48 hours. Valid vulnerabilities will be patched on a private branch and pushed securely. We value responsible disclosure and appreciate your efforts to keep this engine safe.
