CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -ldl -lusb-1.0

LIB = libps_intercept.so
SRC = usb_interceptor.c

.PHONY: all clean

all: $(LIB)

$(LIB): $(SRC)
	$(CC) -shared -fPIC $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(LIB)
