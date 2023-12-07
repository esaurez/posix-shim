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
	LD_LIBRARY_PATH=$(DEMIKERNEL_LIB):$$LD_LIBRARY_PATH $(CC) $(CFLAGS) $(SRC) -fPIC -shared -o $(LIBDIR)/$(SHIM_LIB) -llibdemikernel

clean:
	rm -f $(LIBDIR)/$(SHIM_LIB)
