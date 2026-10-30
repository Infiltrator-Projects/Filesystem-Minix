CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
SRC := native/filesystems/minix/module
OUT ?= build
.PHONY: all test clean
all: $(OUT)/MINIX.so
$(OUT):
	mkdir -p "$(OUT)"
$(OUT)/MINIX.so: $(SRC)/minix_module.c $(SRC)/minix_module.h | $(OUT)
	$(CC) $(CFLAGS) -fPIC -fno-builtin -fno-stack-protector -ffreestanding -fvisibility=hidden -nostdlib -shared -Wl,-z,defs $(SRC)/minix_module.c -o "$@"
$(OUT)/minix-test: tests/minix_dropin_module_test.c $(SRC)/minix_module.c $(SRC)/minix_module.h | $(OUT)
	$(CC) $(CFLAGS) -I$(SRC) tests/minix_dropin_module_test.c $(SRC)/minix_module.c -o "$@"
test: $(OUT)/MINIX.so $(OUT)/minix-test
	$(OUT)/minix-test
	readelf -Ws $(OUT)/MINIX.so | grep -F infiltrator_filesystem_module_v1
	readelf -Ws $(OUT)/MINIX.so | grep -F infiltrator_filesystem_module_v2
	readelf -p .ifsmeta $(OUT)/MINIX.so | grep -F 'IFS-MODULE:minix:2'
	! readelf -d $(OUT)/MINIX.so | grep -q '[(]NEEDED[)]'
clean:
	rm -rf "$(OUT)"
