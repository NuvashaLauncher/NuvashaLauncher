# Accessibility Statement

Nuvasha Launcher aims to be usable by everyone on Linux desktops, following the GNOME Human Interface Guidelines (HIG) and the built-in accessibility capabilities of GTK 4 and libadwaita.

## Supported Accessibility Features

### Keyboard Navigation
The interface can be operated using the keyboard (`Tab`, `Shift+Tab`, `Enter`, `Space`, arrow keys, and `Esc` to close dialogs and popovers).

Global keyboard shortcuts:
- `Ctrl+?` — Open Keyboard Shortcuts dialog
- `Ctrl+N` — Create a new instance
- `Ctrl+F` — Focus search
- `Ctrl+,` — Open Preferences
- `Ctrl+Q` — Quit the application

### Screen Readers & Assistive Technology
- Built on GTK 4's AT-SPI2 / `GtkAccessible` integration for compatibility with Linux screen readers such as **Orca**.
- Icon-only buttons (header actions, row actions, cancel/delete controls, and checkboxes) expose descriptive accessible labels (`GTK_ACCESSIBLE_PROPERTY_LABEL`) and tooltips.
- Navigation sidebars and list rows support standard focus indicators and row activation.

### Visual Adaptation & Internationalization
- **System Theme & High Contrast:** Automatically respects the desktop color scheme (light/dark) and GNOME High Contrast settings via libadwaita (`AdwStyleManager`).
- **Text Scaling & Truncation:** Follows system font scaling; long labels in narrow sidebars ellipsize cleanly with full-text tooltips.
- **Multilingual Support:** Localized into 28 languages/regions via gettext, including RTL-compatible layout handling through GTK 4.

### Headless CLI Alternative
For users who prefer or require terminal/text-based workflows, Nuvasha Launcher provides a scriptable CLI (`nuvashalauncher --help`) for listing instances, managing accounts, inspecting worlds/servers, and launching Minecraft headlessly.

## Known Limitations

- **3D Skin Preview:** The interactive 3D skin viewer uses OpenGL (`GtkGLArea`) and is visual; skin management actions (selecting, uploading, or changing variants) remain accessible via standard controls.
- **Minecraft Game Process:** Once Minecraft itself is launched, in-game accessibility depends on Minecraft's own accessibility settings and installed mods.

## Reporting Accessibility Barriers

If you encounter an unlabeled control, keyboard focus trap, contrast issue, or screen-reader problem:

1. Open an issue on GitHub (`https://github.com/NuvashaLauncher/NuvashaLauncher/issues`) describing the screen/dialog, assistive technology used (e.g., Orca version, Wayland/X11), and steps to reproduce.
2. Or contact the maintainer directly at `nuvashalauncher@gmail.com`.
