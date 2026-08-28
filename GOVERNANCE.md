<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Project Governance

pjson is maintained in the open in the `Pico-Developer/pjson` repository.
Repository maintainers are responsible for technical direction, reviews,
releases, security response, and community moderation.

## Decisions and changes

Bug fixes and focused maintenance changes are decided through pull-request
review. Substantial API, compatibility, security, dependency, or governance
changes should begin with a public issue so constraints and alternatives can be
discussed before implementation. Maintainers seek rough consensus, with the
project's documented user contract, security, maintainability, and test evidence
taking priority. Maintainers make the final call when consensus cannot be reached
and record the reasoning publicly unless confidentiality is required for
security or conduct matters.

Changes are merged by a maintainer after review. Authors should not approve their
own changes when another maintainer is available. In a single-maintainer or
urgent security situation, the author may merge after required CI passes if the
reasoning is recorded in the pull request or, for embargoed work, the private
advisory. Releases follow [`RELEASING.md`](RELEASING.md). Security reports and
conduct incidents use the private routes in [`SECURITY.md`](SECURITY.md) and
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md), respectively.

## Maintainers

Maintainers are expected to review contributions respectfully, disclose relevant
conflicts of interest, protect embargoed reports, keep release and ownership
metadata current, and apply project policies consistently. Maintainer access may
be granted to sustained contributors based on technical judgment, review quality,
reliability, and adherence to the Code of Conduct. The existing maintainers make
that decision through a reviewed repository change.

Inactive maintainers may step down or be removed from ownership metadata after a
reasonable attempt to contact them. Administrative access can be revoked
immediately when needed to protect the project or its users. Governance changes
use the same public review process as other substantial project changes.
