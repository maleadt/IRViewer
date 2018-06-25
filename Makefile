CC  = clang
CXX = clang++

CPPFLAGS += $(shell llvm-config --cppflags)
CXXFLAGS += $(shell llvm-config --cxxflags) -g
LDFLAGS  += $(shell llvm-config --ldflags) -Wl,-rpath -Wl,$(shell llvm-config --libdir)
LDLIBS   += $(shell llvm-config --libs)

ROOT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
export JULIA_BASE_DIR=../julia-dev/base

SOURCES=$(wildcard $(ROOT_DIR)/res/*.ll)
RENDERS=$(SOURCES:.ll=.html)

%.html: %.ll llvm-irview
	$(ROOT_DIR)/llvm-irview $< - | pygmentize -l llvm -f html -O full,linenos=1 -o $@
	sed -i 's/class="err"//g' $@

.PHONY: all
all: llvm-irview $(RENDERS)

.PHONY: clean
clean:
	$(RM) llvm-irview

.PHONY: distclean
distclean: clean
	$(RM) $(RENDERS)
