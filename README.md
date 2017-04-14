QuickEdit (qe) - v0.1.0
=======================

A small editor intended for quick edits.

 - Handles arbitrarily long lines
 - Allows partial file edits on huge files (?)
 - Simple viewer alternative to less

How is it fast?
===============

 - Does not need to load complete files.
 - Minimises dealing/thinking with lines.
 - Does not use fancy editing structures such as ropes/skip-lists.
 - Doesn't do that much.

Installation
============

Requires linux and a C99 compiler.

```
git clone https://github.com/tiehuis/QuickEdit
make
```
