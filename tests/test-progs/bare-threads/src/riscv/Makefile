# Toolchain prefix
CROSS_COMPILE = riscv64-unknown-elf-

# Compiler and tools
CXX = $(CROSS_COMPILE)g++
CC  = $(CROSS_COMPILE)gcc
LD  = $(CROSS_COMPILE)ld
OBJDUMP = $(CROSS_COMPILE)objdump
OBJCOPY = $(CROSS_COMPILE)objcopy

# Architecture-specific flags
ARCH = -march=rv64gc -mabi=lp64

# Compilation flags
CXXFLAGS = $(ARCH) -Wall -O0 -ffreestanding -nostdlib -std=c++20
LDFLAGS  = $(ARCH) -Tlink.ld -nostdlib

# Source files
SRCS = start.S threads.cpp
OBJS = $(SRCS:.cpp=.o)
OBJS := $(OBJS:.cc=.o)
OBJS := $(OBJS:.S=.o)

# Output
OUT_DIR = ../../bin/riscv
TARGET = $(OUT_DIR)/threads
DUMP_DIR = ../../dump/riscv

all: dump

$(TARGET).elf: $(OBJS)
	mkdir -p $(OUT_DIR)
	$(LD) -T link.ld -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(ARCH) -c -o $@ $<
	
dump: $(TARGET).elf
	mkdir -p $(DUMP_DIR)
	$(OBJDUMP) -D -S $(TARGET).elf > $(DUMP_DIR)/threads.asm

bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $(TARGET).bin

clean:
	rm -rf $(OUT_DIR) $(DUMP_DIR)
	rm -f *.o


.PHONY: all clean dump bin