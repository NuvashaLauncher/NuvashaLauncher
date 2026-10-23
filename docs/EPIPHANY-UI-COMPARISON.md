# Epiphany UI comparison

This checklist records the UI architecture choices used while comparing
Nuvasha Launcher with Epiphany.

## Applied patterns

- Static widget hierarchy is declared in GTK Builder resources and kept out
  of controller code where it does not depend on runtime data.
- Dynamic lists, models, async operations, rendering, and Markdown remain in
  C so their ownership and cancellation rules stay explicit.
- Shared CSS is bundled as a GResource and loaded at startup.
- Page and dialog callbacks use object-owned connections where the owner is a
  widget or controller.
- Empty, loading, error, and retry states are represented explicitly instead
  of being inferred from list contents.
- Narrow layouts use Adwaita breakpoints rather than window-size polling.

## Deliberate differences

- Account tokens are stored exclusively in Secret Service and are never read
  from or written to the account JSON file.
- OpenGL skin rendering and Modrinth-specific model behavior remain native C
  integrations rather than being moved into templates.

## Verification

The Makefile test target validates resource bundling, Builder parsing,
stylesheet parsing, custom dialog construction, async task behavior, and
window/dialog ownership.

## Feature matrix

| Concern | Nuvasha | Epiphany reference | Decision |
| --- | --- | --- | --- |
| Static UI | GTK Builder/GResource | GTK Builder templates | Keep templates in resources |
| Runtime lists | C models and factories | GObject models | Keep data-dependent rows in C |
| Async work | Cancellable task scopes | Object-owned async work | Cancel with page/dialog lifetime |
| Responsive layout | Adwaita breakpoints | Adwaita breakpoints | Avoid width polling |
| Styling | Bundled shared CSS | Application CSS resources | Centralize reusable classes |
| Empty/error states | Explicit view states | Explicit view states | Keep loading and failure visible |
| Credentials | Secret Service | Platform secret storage | Never serialize tokens in JSON |
| Rendering | Native OpenGL skin view | Native rendering widgets | Keep rendering outside templates |

Display-backed GTK tests are retained in the normal test suite. ASan marks
the tests that initialize GTK/Pango caches as skipped because those toolkit
allocations outlive the test process and are not application-owned leaks.
