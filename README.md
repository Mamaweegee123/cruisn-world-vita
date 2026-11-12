# Cruis’n World PlayStation Vita experiment

An experimental developer handoff for Vita-specific runtime and graphics work. It is not a qualified playable port.

## Current state

A retrieved physical-device diagnostic log from October 2026 records ROM presence/hash validation, renderer initialization, guest runtime/thread startup, a graphics task, RSP ucode entry, RDP range consumption, a native present/swap, graphics completion, and observation of task 2. The log is not paired with an exact SELF/VPK hash and no display screenshot was retained. It does not prove visible output, menus, controls, audio, or gameplay.

The renderer is a custom CPU-side RDP decode/backend using vitaGL, not RT64. Input polling exists, but usable player input is unverified. The recorded audio path was a null sink.

## Build and tests

This candidate is **not independently buildable**. Its CMake configuration expects shared PC runtime/vendor sources, generated guest functions and RSP output outside this export. See [build requirements](BUILD_REQUIREMENTS.md). No game build or test was run while preparing this candidate.

## Contents and licensing

The scoped MIT notice applies only to included eligible owner-directed Vita source, build glue, and handoff documentation. It does not license the shared GPL runtime, generated game code, external VitaSDK packages, game content, or third-party libraries. See [third-party notices](THIRD_PARTY_NOTICES.md).

## Preservation note

> This repository is an incomplete experimental source handoff, not a full backup of the original development environment.
