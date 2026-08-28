<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Licensing and SPDX Policy

pjson is licensed under the Apache License, Version 2.0. The authoritative
license text is the repository's `LICENSE` file. Do not modify or abbreviate
that canonical text.

The SPDX short identifier for this license is:

```text
Apache-2.0
```

New first-party files should carry an SPDX copyright line and license identifier
in the comment syntax appropriate to the file. For example:

```cpp
// SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates
// SPDX-License-Identifier: Apache-2.0
```

```markdown
<!-- SPDX-FileCopyrightText: 2026 ByteDance Ltd. and/or its affiliates -->
<!-- SPDX-License-Identifier: Apache-2.0 -->
```

Existing files that carry the full Apache-2.0 notice remain valid. The root
`REUSE.toml` supplies Apache-2.0 and ByteDance copyright metadata to first-party
files that do not have complete in-file SPDX information. It also records the
same metadata externally for machine-consumed fuzz corpus files, whose bytes
must not be changed by inserting comments.

Do not perform mechanical license-header churn in generated files or
third-party material. Preserve existing copyright, license, and attribution
notices when modifying or redistributing files. A third-party file must carry
its own complete SPDX information or have a more specific `REUSE.toml`
annotation so that the project's default annotation does not apply to it.

Third-party code, generated distributions, and dependency bundles must retain
their own applicable notices. `CODE_OF_CONDUCT.md` is adapted from Contributor
Covenant 2.1 and remains licensed under CC-BY-4.0, as recorded in `REUSE.toml`;
all other currently tracked project files are licensed under Apache-2.0. A pjson
source or binary distribution must include the root `LICENSE` file and the
applicable texts in `LICENSES/`. If future dependencies require additional
attribution, record it separately without changing the terms of the pjson
license.

Unless explicitly stated otherwise, contributions intentionally submitted for
inclusion in pjson are provided under Apache-2.0, consistent with section 5 of
the license and the contributor requirements in `CONTRIBUTING.md`.
