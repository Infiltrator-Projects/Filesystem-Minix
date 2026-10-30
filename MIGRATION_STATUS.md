# Minix Defragmenter integration
MINIX.so now builds both the existing Minix V1/V2/V3 read-only/existing-file
overwrite interface (V1/V2) and the imported independent Defragmenter native
mutation, journal/recover and Test Media implementation, through a shared
format-neutral host ABI.

The former freestanding Makefile build remains for compatibility verification,
but release consumers must build and install the canonical combined CMake
MINIX.so rather than use the limited old module. Defragmenter integration and
end-to-end tests are required before deleting its original native code.
