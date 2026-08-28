<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Security Policy

## Supported versions

Once 1.0.0 is published, pjson intends to provide security fixes for the current
stable release line. The `1.0.0` source is currently under development, so there
is no supported stable line until `release-1.0.0` is published. Reports against
`main` are welcome and fixes are released as soon as practical.

| Version | Supported |
| --- | --- |
| `main` / 1.0.0 development | Pre-release reports accepted |
| 0.0.x | No |

Users of an unsupported version should upgrade before reporting a problem that
may already be fixed. Reports against `main` are welcome when they identify the
affected commit.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use GitHub's
[private vulnerability reporting](https://github.com/Pico-Developer/pjson/security/advisories/new)
to send the report to the maintainers. If that form is unavailable, open a
public issue containing only a request for a private contact channel; do not
include exploit details, proof-of-concept code, secrets, or affected user data.

Include as much of the following as possible:

- the affected pjson version or commit;
- operating system, compiler, architecture, and relevant build options;
- the vulnerability class and likely impact;
- the smallest reproducible input or proof of concept;
- whether the issue is known to be actively exploited;
- suggested mitigations or fixes, if any; and
- how you would like to be credited.

Encrypt or redact sensitive artifacts before sharing them. Do not submit real
credentials, personal data, or production data.

## What to expect

The maintainers aim to acknowledge a complete report within three business
days and provide an initial assessment within seven business days. These are
response targets, not guarantees. The reporter will receive updates when the
risk assessment, remediation plan, or disclosure schedule changes.

For an accepted vulnerability, maintainers will coordinate a fix, tests, a
security advisory, and a patched release. A CVE will be requested when
appropriate. Credit is given unless the reporter asks to remain anonymous.
Please allow time for supported users to update before publishing details; a
90-day disclosure window is a guideline and may be shortened for active
exploitation or extended by mutual agreement.

## Scope

Security reports may include memory-safety defects, parser or serializer
confusion, validation bypasses, denial-of-service inputs, unsafe default
behavior, or dependency and distribution issues that affect pjson consumers.

The following are normally out of scope:

- unsupported releases when the issue is fixed in a supported version;
- performance differences without a practical denial-of-service impact;
- vulnerabilities in optional third-party benchmark dependencies that do not
  affect pjson; and
- reports that require social engineering or compromised build infrastructure
  outside this repository.

## Good-faith research

Please test only systems and data you are authorized to use, minimize privacy
impact and service disruption, and give the maintainers a reasonable chance to
remediate the issue before disclosure. The project will treat research carried
out under these conditions as good-faith security research.
