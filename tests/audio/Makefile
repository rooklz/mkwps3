CXX ?= c++
ROOT := ../..

ax_decode_test: ax_decode_test.cpp stub/memory.h $(ROOT)/runtime/src/hle/audio/ax_internal.h
	$(CXX) -std=gnu++17 -O1 -Istub -I$(ROOT)/runtime/include -I$(ROOT)/runtime/src/hle/audio $< -o $@

AX_SPU_DEPS := stub/memory.h stub/spu_mfcio.h $(ROOT)/runtime/src/hle/audio/ax_voice.h \
               $(ROOT)/runtime/src/hle/audio/ax_internal.h $(ROOT)/runtime/src/hle/audio/ax_spu_core.h \
               $(ROOT)/platform/ps3/spu/ax_spu.c $(ROOT)/platform/ps3/spu/ax_spu_job.h
ax_spu_test: ax_spu_test.cpp $(AX_SPU_DEPS)
	$(CXX) -std=gnu++17 -O2 -Istub -I$(ROOT)/runtime/include -I$(ROOT)/runtime/src/hle/audio \
		-I$(ROOT)/platform/ps3/spu $< -o $@

.PHONY: run clean
run: ax_decode_test ax_spu_test
	./ax_decode_test
	./ax_spu_test

clean:
	rm -f ax_decode_test ax_spu_test

.DEFAULT_GOAL := run
