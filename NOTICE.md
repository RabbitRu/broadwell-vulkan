# Upstream sources

This experiment derives parts of its driver from Mesa 26.2.7 (HasVK, ELK, ISL,
BLORP and the Vulkan runtime/WSI). Upstream copyright and license notices are
preserved in the generated patch and source-derived files. Original Mesa sources
are downloaded by the bootstrap scripts using the version and SHA-256 in
`tools/dependencies.json`; their licenses remain applicable to those portions.

The unchanged target application is Khronos Vulkan-Tools v1.3.301, built with
pinned Vulkan-Headers and Volk dependencies. Their sources and notices are
downloaded separately rather than vendored in this repository.

Microsoft build tools/SDKs, Intel driver binaries and captured Intel shader code
are not distributed in this repository. This document does not assign a new
project-wide license to original project code.
