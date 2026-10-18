# clotran-ui

User interfaces for [clotran](https://github.com/clotran/clotran) --
Clojure hosted on Fortran: native windows and widgets, a 2D canvas to
draw on, and a 3D view, on macOS, Linux and Windows. It is the platform
other libraries build on; [clotran-plot](https://github.com/clotran/clotran-plot)
is the first.

Built over [libui-ng](https://github.com/libui-ng/libui-ng): one small C
library giving each system's own windows and widgets, with a drawing
area that is the 2D canvas, and whose native view (an NSView, an HWND, a
GtkWidget) is handed to Metal or Vulkan for 3D.

Nothing works yet: what it is to become, and in what order, is
[PLAN.md](PLAN.md).

## License

EPL-2.0 (LICENSE). libui-ng, vendored, is MIT.
