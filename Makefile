# SPDX-License-Identifier: GPL-3.0-or-later
# Build the production Common V2 MINIX.so, not the retired native drop-in ABI.
.PHONY: all test clean
all:
	cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
	cmake --build build/cmake --parallel 2
test: all
	ctest --test-dir build/cmake --output-on-failure
clean:
	rm -rf build/cmake
