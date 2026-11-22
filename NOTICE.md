# License and upstream sources

The original code of this project (the WDDM transport in `native/`, the Windows
platform files `mesa/anv_wddm_*`, the scripts in `tools/`, the tests and the
documentation) is under the MIT License, see [LICENSE](LICENSE).

Parts of the driver derive from Mesa 26.2.7 (HasVK, ELK, ISL, BLORP and the
Vulkan runtime/WSI), which is also under the MIT License. Upstream copyright
and license notices are preserved in `mesa/patches/compiler-port.patch` and in
the source-derived files; those portions stay under their upstream notices.
Original Mesa sources are downloaded by the bootstrap scripts using the version
and SHA-256 in `tools/dependencies.json`.

The binary package additionally contains zlib and small third-party parts of
Mesa under other permissive licenses (Boost, BSD, Apache-2.0). Their notices
are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), which
`tools/gen-third-party-notices.py` generates from the list of sources actually
linked into the driver; the release package ships it together with `LICENSE`.

The unchanged test application is Khronos Vulkan-Tools v1.3.301, built with
pinned Vulkan-Headers and Volk dependencies. Their sources and notices are
downloaded separately rather than vendored in this repository.

Microsoft build tools/SDKs, Intel driver binaries and captured Intel shader code
are not distributed in this repository and are not covered by its license.
