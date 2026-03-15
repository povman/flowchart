# Flowchart Editor

Native **Linux flowchart editor** built with **GTK4** and **Cairo**. It
allows you to create, edit, connect, and export diagrams quickly and
easily.

![Linux](https://img.shields.io/badge/platform-Linux-blue)\
![GTK4](https://img.shields.io/badge/GTK-4-green)\
![License](https://img.shields.io/badge/license-MIT-green)

------------------------------------------------------------------------

# Features

-   **5 shape types:** Rectangle, Rounded Rectangle, Diamond, Circle,
    and Text
-   **Connectors** with configurable arrows (start, end, both, or none)
-   **Resize handles** on corners and edges
-   **Undo/Redo** with `Ctrl+Z` (50 actions history)
-   **Text editing** via double-click or `E` key
-   **Line styles:** Solid, Dashed, Dotted
-   **Color and line width** configurable per shape/connector
-   **Save/Load** projects in `.flow` format (`Ctrl+S` / `Ctrl+O`)
-   **Export diagrams to PNG**

------------------------------------------------------------------------

# Keyboard Shortcuts

  Shortcut         Action
  ---------------- --------------------------
  `Ctrl+Z`         Undo
  `Delete`         Delete selected shape
  `E`              Edit selected shape text
  `Double click`   Edit clicked shape text
  `Ctrl+S`         Save project
  `Ctrl+O`         Open project

------------------------------------------------------------------------

# Dependencies

-   **GTK4** (`libgtk-4-dev`)
-   **Cairo** (included with GTK4)
-   **GCC** and **pkg-config**

### Manjaro / Arch Linux

``` bash
sudo pacman -S gtk4 base-devel
```

### Ubuntu / Debian

``` bash
sudo apt install libgtk-4-dev build-essential
```

### Fedora

``` bash
sudo dnf install gtk4-devel gcc make
```

------------------------------------------------------------------------

# Build and Run

``` bash
git clone https://github.com/your-user/flowchart-editor.git
cd flowchart-editor
make
./flowchart
```

Install system-wide:

``` bash
sudo make install
```

Clean build artifacts:

``` bash
make clean
```

------------------------------------------------------------------------

# `.flow` File Format

Projects are stored in **plain text**, making them easy to version and
inspect manually.

    # Flowchart Project File
    # Version: 1.0
    SHAPES:2
    CONNECTORS:1
    COLOR:0.200,0.600,0.900
    LINE_WIDTH:2.0
    LINE_STYLE:0
    ARROW_TYPE:1

    # Shapes
    SHAPE:0,0,100.0,100.0,100.0,70.0,0.200,0.600,0.900,0.700,"Start",1
    SHAPE:1,0,300.0,100.0,100.0,70.0,0.200,0.600,0.900,0.700,"End",2

    # Connectors
    CONNECTOR:0,1,200.0,135.0,300.0,135.0,0.200,0.600,0.900,1.0,2.0,0,1

------------------------------------------------------------------------

# Code Structure

    flowchart.c   — Main logic, GTK4 callbacks, and main()
    flowchart.h   — Data structures and function prototypes
    Makefile      — Build configuration with separated objects

------------------------------------------------------------------------

# License

This project is licensed under the MIT License.

Copyright (c) 2026 Fábio Moraes

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files to deal in the
Software without restriction.

See the `LICENSE` file for full details.
