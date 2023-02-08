Introduction
============

> fm is a file browser for the terminal.

The main goal is to provide a faster way to explore a file system from
the terminal, compared to what's possible by using `cd`, `ls`, etc.
fm has vi-like key bindings for navigation and can open files in
`$PAGER` and `$EDITOR`.  Basic file system operations are also
implemented (see fm(1) for details).  fm is designed to be simple,
fast and portable.

fm was forked from [Rover](https://github.com/lecram/rover) and is still
heavily a work in progress.  Lots of features were removed that need to
be reimplemented and more need to be fixed.

Quick Start
===========

Building and Installing:

	$ make
	$ doas make install


Running:

	$ fm [dir ...]


Please read fm(1) for more information.


Requirements
============

* Unix-like system;
* curses library.


Copying
=======

All of the source code and documentation for fm is released into the
public domain and provided without warranty of any kind.
