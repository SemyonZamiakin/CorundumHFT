# Contributing to Corundum HFT

Thanks for your interest. Bug reports, benchmark results from real hardware and focused pull requests are welcome.

## Before you start

- For anything larger than a small fix, open an issue first so the approach can be agreed.
- Keep changes focused: one topic per pull request.

## Build and test

```bash
scripts/check.sh
```

This builds with GCC (`-Werror`) and with Clang under AddressSanitizer and UndefinedBehaviorSanitizer, runs every test, and checks that no test segment is left in `/dev/shm`.

Rules for the code:

- The hot path (`crd_reserve`, `crd_commit`, `crd_stage`, `crd_publish`, `crd_read`, `crd_release`, `crd_notify` when nobody sleeps) must stay free of system calls, locks and heap allocation.
- Any change to memory ordering must come with an explanation in [docs/DESIGN.md](docs/DESIGN.md) and must pass CI on arm64.
- Any change to the shared layout must bump `CRD_LAYOUT_VERSION`.
- New behaviour needs a test. Keep the build warning-free.
- Documentation exists in English and Ukrainian; update both, or say in the pull request that the translation is pending.

## Benchmark reports

The most useful benchmark report contains the CPU model, whether it is bare metal or a VM, the kernel version, the compiler, the exact command line (including `-c` pinning) and the full output of `corundum-bench`.

## Style

- Match the surrounding code: naming (`crd_` prefix in C, `corundum::` in C++), formatting and comment density.
- No new third-party dependencies without discussion.
- Do not commit binaries, build output or secrets.

## Commits and pull requests

- Write the commit subject in the imperative mood ("Fix X", "Add Y"), under about 72 characters, with a short body that explains why.
- Describe in the pull request what you tested and on what CPU and kernel.

## License

By contributing you agree that your contribution is licensed under the MIT license of this repository.
