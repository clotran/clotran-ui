# clotran-ui

User interfaces for [clotran](https://github.com/clotran/clotran) --
Clojure hosted on Fortran: native windows and widgets, a 2D canvas to
draw on, and a 3D view, on macOS, Linux and Windows. It is the platform
other libraries build on; [clotran-plot](https://github.com/clotran/clotran-plot)
is the first.

Built over [libui-ng](https://github.com/libui-ng/libui-ng): one small C
library giving each system's own windows and widgets, with a drawing
area that is the 2D canvas, and a 3D view drawn by the GPU over it.

## A window from the REPL

    (require '[clotran.ui :as ui])
    (def w (ui/window {:title "hello" :on-event prn}))
    (ui/draw! w [{:op :rect :x 0 :y 0 :w 600 :h 400 :fill "#0f0f12"}
                 {:op :polyline :points [[10 10] [300 200] [590 20]] :width 3 :colour "#58C4DD"}
                 {:op :text :x 300 :y 350 :text "hello" :size 18 :colour "white" :anchor :middle}])
    (ui/close! w)

A window is a number. Drawing is a **display list** -- plain maps, `:rect`
`:polyline` `:polygon` `:text`, in pixels from the top left -- handed to
the window, which keeps a copy and redraws from it whenever the system
asks; `:layer 1` draws over layer 0 and is replaced on its own (a cursor
over a picture). What the person does comes back as event maps to the
window's `:on-event` function: `:move` `:down` `:up` (with `:x` `:y`),
`:key` (`:char` or `:key`), `:resize`, `:text` (a window made with
`:entry true` has a text field above its drawing area; `entry-text` reads
it), `:left`, `:closed`.

A program that ends with a window open stays alive until the window is
closed, as clj does with a JFrame; `(ui/wait)` waits for that explicitly.

## How it works

- **The main thread.** macOS allows windows only on a process's first
  thread, and clotran evaluates on a second. clotran (core, branch
  `feat/ui-main-thread`, commit 7594d035) lets the first thread serve: `fortran/main-thread*`
  hands it libui-ng's event loop, which runs there until the last window
  closes, and is handed again for the next.
- **No callbacks into clotran.** libui-ng's draw callback replays the copied
  display list in C; its mouse, key, resize and text callbacks push events
  onto a queue a clotran thread drains. A garbage collection never delays a
  frame, and nothing re-enters the evaluator from C.
- **Building C.** The first `(require 'clotran.ui)` checks the vendored
  libui-ng against its sha256 pins (`native/libui-ng.sha256`), builds it
  with the system's C compiler (about 30 s, once) into clotran's cache, and
  opens it as a clotran package: `native/clotran_ui.c` exports the
  `clotran_manifest` and the `NAME_shim(argc, argv)` routines every clotran
  package does. Nothing is built until a window is wanted.

## 3D on the GPU

    (ui/view3d! w)                      ; the area becomes a 3D view: true if there is a GPU
    (ui/scene3d! w {:vertices [[x y z] ..] :colours [..] :triangles [[i j k] ..]
                    :lines [[[x y z] [x y z] colour] ..] :background "#0f0f12"})
    (ui/camera w)                       ;=> {:azimuth -35.0 :elevation 28.0 :zoom 1.0}

Metal on macOS (`native/metal.m`, a CAMetalLayer over the area's view),
Vulkan on Windows (`native/vulkan.c`, a child window over the area that
lets the mouse through). Both take the same mesh and the same camera
(`native/g3.h`), and their shaders are one program in two languages
(`metal.m`'s MSL, `native/shaders/*` in GLSL), so they draw the same
picture: Metal's and Vulkan's renders of a surface differ in 9 of 3920
sampled pixels. Dragging turns the view and scrolling zooms it in C, with
no trip through clotran. clotran-plot's `show3d` is its first user.

The SPIR-V in `native/shaders/*.h` is generated from the GLSL beside it
by glslang 16.6.0 (`glslangValidator -V --vn spv_NAME`; BSD-3-Clause),
so a build needs no shader compiler. Building on Windows needs MSYS2's
`mingw-w64-ucrt-x86_64-vulkan-headers` and `-vulkan-loader` (Apache-2.0);
running needs only the driver's `vulkan-1.dll`.

## Tests

`clotran-plot`'s `test/window_check.bb` opens real windows and drives them
with `inject!`ed events. `snapshot` replays a window's display list into
an offscreen picture (PNG on macOS; on Windows a BMP, through libui-ng's
own Direct2D drawing in software, which needs no display), `snapshot3d`
renders the 3D view offscreen on the GPU, and `capture` reads back what
the window shows (its frame and widgets on macOS, the shown frame for a
3D view on both).

## Status

macOS (arm64) and Windows 10 (x86_64, MSYS2 UCRT64): windows, the
display-list canvas, events, the text field, and the 3D view -- 14 of 14
window checks on each. Linux (GTK 3) has its build line written,
untested, and no 3D yet. Next: tick labels in the 3D view, and a
prebuilt DLL so a Windows machine without MSYS2 can open windows; see
[PLAN.md](PLAN.md).

## License

EPL-2.0 (LICENSE). libui-ng, vendored under `native/libui-ng`, is MIT
(`native/libui-ng/LICENSE.md`, `native/PROVENANCE.md`).
