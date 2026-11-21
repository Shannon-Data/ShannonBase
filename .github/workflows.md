# ShannonBase Workflow

CI lives in [./workflows](./workflows). Four workflows cover the four official
test levels (`mysql-test/collections/default.{push,daily,weekly,release}`).

| Workflow | Triggers | Jobs |
| --- | --- | --- |
| [pull-requests.yaml](./workflows/pull-requests.yaml) | push to non-`main` branches, pull requests (Docs/markdown excluded) | lint, then x86/ARM Release(ASAN) and Debug builds, Rapid gunit tests, MTR suites (incl. `group_replication` and a ps-protocol run) |
| [nightly.yaml](./workflows/nightly.yaml) | Daily 04:30 Asia/Shanghai | same matrix, nightly MTR |
| [weekly.yaml](./workflows/weekly.yaml) | Saturday 06:30 Asia/Shanghai | same matrix, weekly MTR |
| [release.yaml](./workflows/release.yml) | creation of a new tag containing `v` (release tag), manual dispatch | lint, Debug x86 build, `collections/default.release` |

Tag pushes now run only `release.yml`; they used to trigger `nightly.yaml` and
`weekly.yaml` simultaneously. The release workflow listens to the `create`
event, so only a newly created `*v*` tag starts it, not a re-pushed or moved tag.

Notes:

- Official collections are executed line by line with execv() semantics (arguments split
  on whitespace), so `|` inside `--exclude-platform=...` is not a shell pipe. The runner
  injects the host `--parallel` and `--mysqld=--loose-rapid_schema_embedding=OFF`, and keeps
  `--retry=3 --report-unstable-tests` because flaky tests are currently frequent; MTR only
  hard-fails when a failure is reproducible across retries.
- `lint` checks only changed lines under `storage/rapid_engine` and `ml` with clang-format 18
  (the newest version both Ubuntu 24.04 and 26.04 ship), passing `-binary` so the pinned
  tool is what actually formats. Locally: `sudo apt install clang-format-18`, then run
  `clang-format-18 -i` on your files (or `git clang-format-18`) before pushing. Lint blocks
  merge only for `pull_request` events, so a formatter hiccup never skips the build/test jobs.
- ARM jobs use `ubuntu-24.04-arm`; GitHub has no `ubuntu-latest-arm` label, so the image
  must be bumped by hand when that image is retired.
- Build jobs install their own toolchain dependencies with unversioned `apt` packages and
  install model assets via `python3 -m pip` (PEP 668 compatible when needed).