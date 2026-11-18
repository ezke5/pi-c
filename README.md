<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# pi

Phase 1 provides the C23 foundation for `pi`: configuration and USE-flag
resolution, deterministic SHA-256 cache keys, package metadata output, a
dependency filter, and a Linux namespace sandbox entry point.

Build with Clang and run the focused tests:

```sh
make
make test
```