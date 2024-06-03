# The LLVM Compiler Infrastructure

This repository houses versions of LLVM that support downgrading IR to previous versions.
Specifically, LLVM 5.0 and LLVM 7.0, as required to target respectively Apple's Metal and
NVIDIA's NVVM.

The actual code does not live on the `main` branch, but in branches versioned after the
LLVM version they contain: `downgrade_release_13` to `downgrade_release_17`.

We currently only aim to support the LLVM versions that are used by the Julia programming
language. In addition, these branches may contain additional patches as used by Julia.
