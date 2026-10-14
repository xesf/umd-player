# umd-player

A native PS Vita app that plays UMD Video discs from ISO dumps. It reads the disc structure, decodes H.264 video with the Vita hardware decoder, and decodes ATRAC3plus audio in software. It does not need Adrenaline or the PSP firmware.

Status: planning. No code yet.

- [Build plan](docs/plan.md)
- [UMD Video format reference](docs/formats/umd-video.md)
- [Licensing](docs/licensing.md)

Requirements: VitaSDK. The app uses no other libraries.

Licence: MIT, except `src/core/atrac3p/`, which is LGPL-2.1-or-later. See [docs/licensing.md](docs/licensing.md).
