CC := /workspace/sdk/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc

CFLAGS := -O2 -Wall -Wextra
TARGET := build/crow_camera

all: $(TARGET)

$(TARGET): src/main.c
	mkdir -p build
	$(CC) $(CFLAGS) -static src/main.c -o $(TARGET)

clean:
	rm -rf build