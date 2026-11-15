# ShannonBase Workflow

CI lives in [./workflows](./workflows). There is no separate `release.yml`; builds are
driven by the three workflows below.

| Workflow | Triggers | Jobs |
| --- | --- | --- |
| [pull-requests.yaml](./workflows/pull-requests.yaml) | push to non-`main` branches, pull requests (Docs/markdown excluded) | lint, then x86/ARM Release(ASAN) and Debug builds, Rapid gunit tests, MTR suites |
| [weekly.yaml](./workflows/weekly.yaml) | Saturday 06:30 Asia/Shanghai, tag push matching `*v*` | same matrix, weekly MTR collection |
| [nightly.yaml](./workflows/nightly.yaml) | Daily 04:30 Asia/Shanghai, tag push matching `*v*` | same matrix, nightly build and MTR |

Notes:

- `lint` checks only changed lines under `storage/rapid_engine` and `ml` with clang-format 18
  (the newest version both Ubuntu 24.04 and 26.04 ship), passing `-binary` so the pinned
  tool is what actually formats. Locally: `sudo apt install clang-format-18`, then run
  `clang-format-18 -i` on your files (or `git clang-format-18`) before pushing. Lint blocks
  merge only for `pull_request` events, so a formatter hiccup never skips the build/test jobs.
- ARM jobs use `ubuntu-24.04-arm`; GitHub has no `ubuntu-latest-arm` label, so the image
  must be bumped by hand when that image is retired.
- Build jobs install their own toolchain dependencies with unversioned `apt` packages and
  install model assets via `python3 -m pip` (PEP 668 compatible when needed).