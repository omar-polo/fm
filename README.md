Introduction
============

> fm is a file browser for the terminal.

The main goal is to provide a faster way to explore a file system from
the terminal, compared to what's possible by using `cd`, `ls`, etc.  fm
has vi-like key bindings for navigation and can open files in $PAGER and
$EDITOR.  Basic file system operations are also implemented (see fm(1)
for details).  fm is designed to be simple, fast and portable.

fm was forked from [Rover](https://github.com/lecram/rover).

Quick Start
===========

 Building and Installing:
 ```
 $ make
 $ sudo make install
 ```

 Running:
 ```
 $ fm [DIR1 [DIR2 [DIR3 [...]]]]
 ```

 Basic Usage:
 ```
       q - quit fm
       ? - show fm manual
     j/k - move cursor down/up
     J/K - move cursor down/up 10 lines
     g/G - move cursor to top/bottom of listing
       l - enter selected directory
       h - go to parent directory
       H - go to $HOME directory
     0-9 - change tab
  RETURN - open $SHELL on the current directory
   SPACE - open $PAGER with the selected file
       e - open $VISUAL or $EDITOR with the selected file
       / - start incremental search (RETURN to finish)
     n/N - create new file/directory
       R - rename selected file or directory
       D - delete selected file or (empty) directory
 ```

 Please read fm(1) for more information.


Requirements
============

 * Unix-like system;
 * curses library.


Configuration
=============

fm configuration (mostly key bindings and colors) can only be changed by
editing the file `config.h` and rebuilding the binary.

Note that the external programs executed by some fm commands may be
changed via the appropriate environment variables. For example, to
specify an editor:

```
$ VISUAL=vi fm
```

fm will first check for variables prefixed with ROVER_. This can be used
to change fm behavior without interfering with the global environment:

```
$ ROVER_VISUAL=vi rover
```

Please read fm(1) for more information.


Copying
=======

All of the source code and documentation for fm is released into the
public domain and provided without warranty of any kind.
