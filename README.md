# dutop (1.0.1)

A simple, robust, and visually pleasing UNIX disk usage analyzer. 

Originally written a long time ago as a learning project, recently rediscovered, completely refactored, and documented. It combines the functionality of `du` with a clear, top-N visual ASCII bar graph. Feel free to use it, learn from it, or modify it.

## Features

* **Iterative, Not Recursive:** Uses a custom heap-allocated LIFO stack instead of recursion. It shall never* crash with a `Segmentation fault`, even on directory structures that are tens of thousands of levels deep.
* **Symlink Safe:** Uses `AT_SYMLINK_NOFOLLOW` to prevent infinite loops.
* **Filesystem Aware:** Can isolate scans to a single filesystem/device (ignoring virtual filesystems like `/proc` or `/run`).
* **Memory Safe:** Built with defensive programming principles and strict error handling. 100% clean Valgrind record with zero memory leaks on common happy path scenarios and most probable errors.

\* at least I hope.

## What it does

**dutop** scans a given directory, calculates the total size of its contents, and displays the top largest items in a clear, formatted bar graph.

By default it is pretty verbose.

```text
$ dutop -sx /

   158.55 GB [################################] home
    13.61 GB [##                              ] usr
     5.19 GB [#                               ] var
   705.06 MB [                                ] opt
   664.70 MB [                                ] boot
   477.69 MB [                                ] root
     6.86 MB [                                ] etc
     7.76 MB [                                ] ...and 15 others.
   180.11 GB TOTAL
```

## What it does NOT do

* It does not run in parallel. It is single-threaded. While fast and efficient for daily use, it is not optimized for multi-threaded scanning of massive enterprise NAS arrays.
* It does not follow symlinks. This is an intentional design choice to prevent infinite traversal loops.
* It does not rely on the OS call stack. (See the Iterative, Not Recursive feature above).

## Usage

```
Usage: dutop [-n count] [-w barwidth] [-isx] [-d | -f] [directory]
Options:
  -n count     Number of entries to show (0 for all, default: 10)
  -w barwidth  Width of the bar graph excluding [] brackets (default: 32)
  -s           Silent mode (suppress errors)
  -x           Stay on one file system (ignore /proc, /sys, USBs, etc.)
  -i           Ignore hidden files/directories
  -d           Show only directories (exclusive with -f)
  -f           Show only files (exclusive with -d)
  -V           Show version information and exit
  -h           Show this very helpful message

Default directory is current working directory.
```

### Examples

* Find the top 5 largest directories in the current folder: `dutop -n 5`
* Scan the entire root drive silently, ignoring virtual file systems (like `/proc`) and external drives: `sudo dutop -sx /`
* Show a detailed, wide graph (60 chars) of all items in `/var/log`: `dutop -n 0 -w 60 /var/log`

## Installation

You can compile and install **dutop** directly from the source code. You will need standard C development tools (like `gcc` or `clang`).

#### 1. Clone:

```
git clone https://github.com/w3n11/dutop.git
cd dutop
```

#### 2. Compile:

```
gcc -std=c99 -Wall -Wextra -Werror -pedantic -O2 -o dutop dutop.c
```
or run:
```
./compile.sh
```

#### 3. Make it available system-wide

```
sudo cp dutop /usr/local/bin/
```

## Memory safety & Testing

This tool is tested on:

* 1 000 nested directories
* 10 000 files in a single directory
* artificially restricted memory limit of 3 000 kB
* restricted, unreadable and inaccessible directories

All above ended up with proud

```text
==29942== HEAP SUMMARY:
==29942==     in use at exit: 0 bytes in 0 blocks
==29942== All heap blocks were freed -- no leaks are possible
==29942== ERROR SUMMARY: 0 errors from 0 contexts
```

---

Enjoy. <3