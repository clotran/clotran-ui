# native/libui-ng: provenance

- **What:** libui-ng, "a portable GUI library for C" -- native windows and
  widgets over Cocoa (macOS), GTK 3 (Linux) and Win32 (Windows).
- **From:** https://github.com/libui-ng/libui-ng at commit
  `43ba1ef553c8993a43a67f1ce6e35983a2660d8c` (master, 2025-03-15),
  downloaded as
  https://codeload.github.com/libui-ng/libui-ng/tar.gz/43ba1ef553c8993a43a67f1ce6e35983a2660d8c
  whose sha256 is
  `55315ed938d13ab82fe3fb42a6cc2eccfd3a5991750f7e00f841d4f2403e34dd`.
  Upstream publishes no digest of its own; the pin is ours.
- **Licence:** MIT (`libui-ng/LICENSE.md`, copied unmodified).
- **Kept:** `LICENSE.md`, `ui.h`, `ui_darwin.h`, `ui_unix.h`,
  `ui_windows.h`, and the `common/`, `darwin/`, `unix/` and `windows/`
  source trees, each file byte for byte as in the tarball (their
  `meson.build` files too: clotran-ui reads its source lists from them).
- **Left out:** `examples/`, `test/`, `doc/`, `old/`, `subprojects/`, the
  top-level meson files, `Doxyfile`, the README and changelogs.
- **Checked:** `libui-ng.sha256` holds every kept file's sha256;
  `clotran.ui` checks the tree against it (`shasum -c` or `sha256sum -c`)
  before it builds, and refuses to build on a mismatch.
- **Modified:** nothing. clotran-ui's own C is `clotran_ui.c` and
  `darwin.m`, beside the tree.
