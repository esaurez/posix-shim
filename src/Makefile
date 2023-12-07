# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

#=======================================================================================================================
# Build Artifacts
#=======================================================================================================================

# Source Files
SRC=$(wildcard *.c) $(wildcard ctrl/*.c) $(wildcard data/*.c) $(wildcard epoll/*.c)

#=======================================================================================================================

all: all-libs

all-libs:
	mkdir -p $(LIBDIR)
	$(CC) $(CFLAGS) -L$(DEMIKERNEL_LIB) -fPIC -shared -o $(LIBDIR)/$(SHIM_LIB) $(SRC) -ldemikernel 

clean:
	rm -f $(LIBDIR)/$(SHIM_LIB)
