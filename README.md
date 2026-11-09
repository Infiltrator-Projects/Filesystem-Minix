# Filesystem-Minix
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

The published portable Debian package installs two regular files:

- `/System/Filesystems/MINIX.so`
- `/System/Resources/Documentation/Filesystem-Minix.pdf`

A manual native RUN builds those files and, when run outside `--system-package-mode` with a prepared kernel build, can also include `/System/Kernel/Filesystems/minix.ko` in its local package. It uses the running kernel headers by default; use `--kernel-build DIRECTORY` for a different prepared kernel tree with `Module.symvers`. The optional kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.
