CXX ?= c++
ROOT := ../..

ax_decode_test: ax_decode_test.cpp stub/memory.h $(ROOT)/runtime/src/hle/audio/ax_internal.h
	$(CXX) -std=gnu++17 -O1 -Istub -I$(ROOT)/runtime/include -I$(ROOT)/runtime/src/hle/audio $< -o $@

.PHONY: run clean
run: ax_decode_test
	./ax_decode_test

clean:
	rm -f ax_decode_test

.DEFAULT_GOAL := run
