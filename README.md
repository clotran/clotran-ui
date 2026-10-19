# clotran-ui

User interfaces for [clotran](https://github.com/clotran/clotran) --
Clojure hosted on Fortran: native windows and widgets, a 2D canvas to
draw on, and a 3D view, on macOS, Linux and Windows. It is the platform
other libraries build on; [clotran-plot](https://github.com/clotran/clotran-plot)
is the first.

Built over [libui-ng](https://github.com/libui-ng/libui-ng): one small C
library giving each system's own windows and widgets, with a drawing
area that is the 2D canvas.

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
  `feat/ui-main-thread`) lets the first thread serve: `fortran/main-thread*`
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

## Tests

`clotran-plot`'s `test/window_check.bb` opens real windows and drives them
with `inject!`ed events. `snapshot` replays a window's display list into
an offscreen PNG and `capture` reads the window itself back, frame and
all (macOS, for now).

## Status

macOS (arm64) works. Linux (GTK 3) has its build line written, untested;
Windows is next. 3D (Metal, Vulkan) follows; see [PLAN.md](PLAN.md).

## License

EPL-2.0 (LICENSE). libui-ng, vendored under `native/libui-ng`, is MIT
(`native/libui-ng/LICENSE.md`, `native/PROVENANCE.md`).
