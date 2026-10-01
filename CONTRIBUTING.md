# Contributing to Facility Efficiency Ledger

Facility Efficiency Ledger is a Data Center Control Plane (DCCP) repository
maintained by Summon Software Labs. Contributions from individuals and
organizations are welcome under the terms of the Apache License 2.0.

## Licensing of contributions

By submitting a contribution you agree that it is licensed under the Apache
License 2.0, as described in section 5 of the [LICENSE](LICENSE). There is **no**
Contributor License Agreement to sign and no copyright assignment. You keep the
copyright to your contribution.

Do not add `Co-authored-by` trailers, generated-by notices, or attribution lines
that you cannot justify. Commit authorship is recorded by Git itself.

## Scope and boundaries

Keep changes inside the repository's documented systems boundary: durable
accounting, classification, attribution, reconciliation, historical generations,
and explainable residuals for facility efficiency. This repository does not own
energy delivery, capacity, pricing, scheduling, placement, policy, or actuation,
and it never infers another runtime's authority from evidence that happens to be
visible to it. Changes that pull those concerns into this repository will be
declined.

Two properties are non-negotiable and any change must preserve them:

* accounting closes exactly, and inputs reconcile to classified outputs plus
  explicit unknown and unmeasured residuals;
* unknown consumption is never folded into useful work or waste by default.

## Before you open a pull request

1. Build both configurations with warnings as errors:

   ```
   cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build/release
   cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build/debug
   ```

2. Run the complete test suite in both configurations:

   ```
   ctest --test-dir build/release --output-on-failure
   ctest --test-dir build/debug --output-on-failure
   ```

   The suite spawns real child processes and kills writers mid-append on purpose.
   It contains no timeouts: if a test hangs, that is a defect to diagnose rather
   than a flake to retry.

3. Run the analyser configuration where your toolchain supports it:

   ```
   cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFEL_ENABLE_ASAN=ON
   cmake --build build/asan
   ctest --test-dir build/asan --output-on-failure
   ```

4. Add the tests that prove your change. Unit tests alone are not sufficient for
   anything touching persistence, recovery, locking, or accounting closure.

## Code quality expectations

* C++20, no compiler extensions, no new third-party dependencies without a
  written justification in the pull request.
* First-party code must compile with `/W4 /WX` on MSVC and
  `-Wall -Wextra -Wpedantic -Werror` elsewhere.
* Every fallible public entry point returns a `Result` or `Status` carrying a
  stable `ReasonCode`; do not add exceptions to the public API and do not add a
  reason code without a test that observes it.
* New persistent fields require a format version decision, a recovery story, and
  an adversarial test that corrupts them.
* Keep the boundary honest: if a claim cannot be proved with the hardware and
  processes available, label it `UNSUPPORTED` in documentation rather than
  implying it.
