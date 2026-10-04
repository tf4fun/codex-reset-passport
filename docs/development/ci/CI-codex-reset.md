<p align="right"><a href="CI-codex-reset.zh_CN.md">简体中文</a></p>

# Codex Reset private CI

The private derivative uses `.github/workflows/codex-reset.yml` on pushes, pull requests and manual dispatch. The original upstream workflows are retained for reference but restricted to the official upstream repository; they do not sync or publish this derivative.

The job uses the official `espressif/idf:v5.5.3` version-tagged container, full-SHA-pinned official GitHub Actions, and read-only repository permissions. It needs no custom secrets, PATs, Wi-Fi data or private service credentials. Checkout authentication is not persisted. The runner aligns only the disposable checkout ownership with the container user before validation. The runner installs host GCC/build tools and curl; Python, CMake, Ninja and the RISC-V toolchain come from the IDF image. The tag pins the SDK version, not an immutable OCI digest; this is a repeatable validation recipe, not a byte-for-byte reproducible build claim.

The hash-guarded application-specific BLUFI patch is applied before the complete `./tools/validate.sh` gate. That gate includes repository/workflow checks, generated-font coverage, all host tests, isolated firmware compilation, merged-image/partition verification and matching debug archive verification. `dependencies.lock` must remain unchanged. Installed assistant skills, npm, fonttools and host font packages are not needed for font checking; the checked-in generated C fonts are used. Regenerating fonts is a separate documented developer operation.

Artifacts contain only the merged binary, SHA256SUMS, provenance manifest and FLASHING.txt. The SDK, local caches, ELF/MAP files and source ZIPs are not uploaded. The manifest lists matching debug-file identities, but those files remain runner-local. Artifacts expire after 14 days. Verify the exact successful commit/run and SHA-256 before download and flashing. Device tests are separate; a merged flash at offset 0x0 may reset NVS settings.

For a source ZIP checkout, extract it, run `git init`, then run validation in an activated ESP-IDF 5.5.3 environment after applying the documented BLUFI patch. Git metadata is required by repository checks. No machine-specific assistant skill links belong in the ZIP. All upstream and font/vendor licenses are retained.
