CC = gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -pthread
LDFLAGS = -lusb-1.0 -lm -lpthread

# Library (shared + static)
LIB = libpicoscope2204a.so
LIB_A = libpicoscope2204a.a
LIB_SRC = picoscope2204a.c
LIB_HDR = picoscope2204a.h
LIB_OBJ = picoscope2204a.o

# Test program
TEST = test_driver
TEST_SRC = test_driver.c

.PHONY: all clean test

all: $(LIB) $(LIB_A) $(TEST)

$(LIB): $(LIB_SRC) $(LIB_HDR)
	$(CC) -shared -fPIC $(CFLAGS) -o $@ $(LIB_SRC) $(LDFLAGS)

# Static archive (used by the Wails GUI via CGo)
$(LIB_OBJ): $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) -c $(LIB_SRC) -o $@

$(LIB_A): $(LIB_OBJ)
	ar rcs $@ $(LIB_OBJ)

$(TEST): $(TEST_SRC) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) -L. -lpicoscope2204a $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

# Run test (requires root for USB access unless udev rule is set).
# Firmware must be installed in $HOME/.config/picoscope-libusb/firmware/
# (see docs/firmware-extraction.md).
test: $(TEST)
	sudo LD_LIBRARY_PATH=. ./$(TEST)

clean:
	rm -f $(LIB) $(LIB_A) $(LIB_OBJ) $(TEST)
