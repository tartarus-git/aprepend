undefine MAIN_CPP_INCLUDES

BINARY_NAME := aprepend

CPP_STD := c++20
OPTIMIZATION_LEVEL := O3
USE_WALL := true
ifeq ($(USE_WALL), true)
	POSSIBLE_WALL := -Wall
else
	undefine POSSIBLE_WALL
endif

CLANG_PREAMBLE := clang++-11 -std=$(CPP_STD) -$(OPTIMIZATION_LEVEL) $(POSSIBLE_WALL) -fno-exceptions

EMIT_ASSEMBLY := false

.PHONY: all unoptimized clean

all: bin/$(BINARY_NAME)

unoptimized:
	$(MAKE) OPTIMIZATION_LEVEL:=O0

bin/$(BINARY_NAME): bin/main.o
	$(CLANG_PREAMBLE) -o bin/$(BINARY_NAME) bin/main.o

bin/main.o: main.cpp $(MAIN_CPP_INCLUDES) bin/.dirstamp
	$(CLANG_PREAMBLE) -c -o bin/main.o main.cpp
	ifeq ($(EMIT_ASSEMBLY), true)
		$(CLANG_PREAMBLE) -S -I. -o bin/main.s main.cpp
	endif

bin/.dirstamp:
	mkdir -p bin
	touch bin/.dirstamp

clean:
	@echo 'removing every untracked (by git) file, including *.swp files, so be careful...'
	git clean -fdx
