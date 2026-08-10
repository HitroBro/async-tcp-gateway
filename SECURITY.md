# Security Policy

## Supported Versions

We release security updates for the following versions:

| Version | Supported          |
| ------- | ------------------ |
| 0.1.x   | :white_check_mark: |

## Reporting a Vulnerability

We take security vulnerabilities seriously. If you discover a security vulnerability in this project, please report it responsibly:

### Private Disclosure (Preferred)

**Email:** ghiahitarth@gmail.com

Please include:
- Description of the vulnerability
- Steps to reproduce
- Potential impact
- Any suggested fixes

We will acknowledge receipt within 48 hours and provide a status update within 5 business days.

### Public Disclosure

If you prefer, you may open a GitHub issue with the `security` label. However, for critical vulnerabilities, private disclosure is strongly encouraged.

## Security Features

This project implements the following security measures:

- **Path Traversal Protection:** Configuration parser validates all paths
- **Rate Limiting:** Token bucket algorithm (global + per-IP)
- **Buffer Overflow Protection:** Fixed-size ring buffers with bounds checking
- **Connection Limits:** Configurable maximum concurrent connections
- **Input Validation:** Strict parsing of IP addresses, ports, and configuration values

## Scope

This policy applies to the async-tcp-gateway codebase only. Test tools (`tests/`) and CI/CD configuration are out of scope for security vulnerability reporting.

## Acknowledgments

We thank all security researchers who responsibly disclose vulnerabilities.