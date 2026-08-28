<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# How to Contribute

We'd love to accept your patches and contributions to this project. There are
just a few small guidelines you need to follow.

## Contribution license

This project does not require a separate contributor license agreement. Unless
you explicitly state otherwise, any contribution intentionally submitted for
inclusion in pjson is licensed under Apache-2.0, as described by section 5 of
the project's [`LICENSE`](LICENSE). You represent that you have the right to
submit the contribution under those terms. Preserve applicable copyright and
attribution notices and see [`LICENSING.md`](LICENSING.md) for repository policy.

## Changes Accepted

Please file issues before doing substantial work; this will ensure that others
don't duplicate the work and that there's a chance to discuss any design issues.

Changes only tweaking style are unlikely to be accepted unless they are applied
consistently across the project. Code style is enforced by the checked-in
`.clang-format` and `.clang-tidy` configurations — run `./build.sh --format` to
apply formatting and `./build.sh --tidy` for static analysis. See
[`docs/10-contributing.md`](docs/10-contributing.md) for the full contributor
guide. Improvements to code structure and clarity are welcome, but please file
issues to track substantial work first.

### Readability expectations

- Give each function a concise purpose or contract comment. For overload
  families and trivial callbacks, one shared comment may cover the group when
  individual comments would only repeat signatures.
- Explain invariants and tradeoffs in complex code: ownership, allocator
  provenance, rollback/atomicity, resource budgets, iterative traversal state,
  standards edge cases, and security limits are more valuable than narration
  of individual statements.
- Organize large implementation files with named subsystem headings. Avoid
  anonymous separator bars and comments that merely translate syntax into
  English.
- Keep comments synchronized with behavior and standards references. A stale
  comment is a correctness defect, not harmless documentation drift.
- Edit the canonical public header and implementation under `pjsonlib/`.

## AUTHORS file

If you would like to receive additional recognition for your contribution, you
may add yourself (or your organization) to the AUTHORS file. This keeps track of
those who have made significant contributions to the project. Please add the
entity who owns the copyright for your contribution. The source control history
remains the most accurate source for individual contributions.

## Pull Requests

We actively welcome your pull requests.

1. Fork the repo and create your branch from `main`.
2. If you've added code that should be tested, add tests.
3. If you've changed APIs, update the documentation.
4. Ensure the full sweep passes: `./build.sh` (equivalently `--all`, which runs
   the formatting check, Release + sanitized Debug builds, all registered test
   cases, comparison benchmarks, bounded fuzz corpus replay when supported, the
   Doxygen reference, relocatable static/shared install and pkg-config checks,
   SPDX/REUSE licensing validation, and clang-tidy). It offers to fetch both
   pinned conformance corpora; `--auto`
   accepts those downloads. An explicit `./build.sh --fuzz` request is strict and
   fails if a usable Clang/libFuzzer toolchain is unavailable.
5. Make sure your code is formatted (`./build.sh --format`).
6. Run the focused documentation or package checks in
   [`docs/10-contributing.md`](docs/10-contributing.md) when those areas change.
7. Add a concise [`CHANGELOG.md`](CHANGELOG.md) entry for a notable
   user-visible change.
8. When adding files or changing license metadata, run `reuse lint`; see
   [`LICENSING.md`](LICENSING.md) for the repository's annotation policy.

## Issues

We use GitHub issues to track public bugs. Please ensure your description is
clear and has sufficient instructions to be able to reproduce the issue.

Do not report suspected vulnerabilities in a public issue. Follow
[`SECURITY.md`](SECURITY.md) for private reporting instead.

## Project policies

The repository keeps its public maintenance and governance information in these
top-level files:

- [`SECURITY.md`](SECURITY.md) explains supported versions, private
  vulnerability reporting, and coordinated disclosure.
- [`VERSIONING.md`](VERSIONING.md) defines compatibility, version sources, and
  tag naming.
- [`RELEASING.md`](RELEASING.md) is the maintainer release checklist.
- [`CHANGELOG.md`](CHANGELOG.md) records notable user-visible changes.
- [`GOVERNANCE.md`](GOVERNANCE.md) describes maintainer responsibilities and
  project decision making.
- [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) defines community standards and
  the private reporting route for conduct incidents.
- [`LICENSING.md`](LICENSING.md) explains licensing and SPDX requirements; the
  canonical license text is in [`LICENSE`](LICENSE).
- [`AUTHORS`](AUTHORS) and [`CONTRIBUTORS`](CONTRIBUTORS) record project
  authorship and contributor recognition.
