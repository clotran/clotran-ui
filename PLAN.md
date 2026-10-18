# clotran-ui: the plan

clotran's PLAN.md step 37. James, 2026-10-18: libui-ng "is meant to
build uis on" -- 2D windows, 3D windows, widgets -- so it is a library of
its own, and plotting (clotran-plot) another on top of it.

## What it is for

The things it must make possible, from James's own list, in the order
they are wanted:
- a window you can draw in from the REPL -- the first thing;
- clotran-plot's graphs in a window, with trace (hover to read a value)
  and zoom (drag a box), as a TI-85 did them;
- live drawing: a view of an atom redraws when the atom changes, so a
  running calculation shows itself;
- 3D: a surface (a potential energy surface), molecules in ball and
  stick, rotated with the mouse;
- linked views: a click in one (a point on a PES) opens another (that
  point's optimization, animated);
- widgets enough for a small tool: buttons, sliders, text fields,
  tables, menus, file dialogs; later, games.

## Decisions

- **libui-ng**, decided 2026-10-10 (not raylib, not SDL): native
  widgets, one C library with a small C API, a drawing area (paths,
  fills, strokes, text, mouse and keyboard, DPI aware) as the 2D canvas.
  Its C source is vendored at a pinned commit, checked by sha256, and
  built as a package the first time something loads it -- so a program
  that never opens a window never builds it.
- **3D** is the drawing area's native view handed to Metal (macOS, an
  Objective-C shim) or Vulkan (Linux, Windows); libui-ng keeps the
  window, the widgets and the events.
- **Clojure in, native out**: a window, a canvas, a widget is a Clojure
  value; drawing is a function of state, called again when the state
  changes. Nothing of libui-ng's API leaks to a user.

## Open questions, settled first

1. **The main thread.** macOS allows windows only on a process's main
   thread, and clotran evaluates on a second one (main.f90's main thread
   only waits for it). The event loop must run on that main thread with
   the REPL's thread sending it work: what does core need -- a way to run
   a function there and wait for its answer?
2. **Callbacks into clotran.** libui-ng calls C functions back (draw this
   area, this button was clicked). They must reach a Clojure function,
   on the main thread, while the evaluator may be busy on another: what
   core needs for a C callback to call into the evaluator safely.
3. **Building C.** A clotran package builds Fortran today (`clotran
   bind`); libui-ng is C (and Objective-C on macOS), linked against each
   system's GUI libraries (Cocoa, GTK 3, Win32). How a package names C
   sources and system frameworks.
4. **The frame loop.** At 60 frames a second the collector's pauses
   show; the collector-pause test runs first (clotran's PLAN, 2026-10-10).

Each answer that needs core is a change there, gated and with its own
oracle case, as clotran's own work is.

## Milestones

1. A window from the REPL, a canvas in it, lines and text drawn into it,
   closed cleanly -- on macOS first, then Linux and Windows.
2. Mouse and keyboard events reaching Clojure functions.
3. Redraw from state: a canvas over an atom.
4. Widgets: button, slider, text field, then tables and menus.
5. The 3D view: a Metal view on macOS showing a triangle, then a lit
   surface; Vulkan on Linux and Windows.
6. Tested like the rest of clotran: headless where it can be (drawing
   into an image and comparing it), and in a real window on each system.
