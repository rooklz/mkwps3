CC ?= cc
SRC := ../../renderer/src
CFLAGS := -std=gnu11 -O0 -g -DFP_TRACE -I$(SRC) -w

tevdump: tevdump.c $(SRC)/video/rsx/tev_program.c $(SRC)/core/gx/bp.c $(SRC)/video/rsx/gx_features.c $(SRC)/common/log.c
	$(CC) $(CFLAGS) $^ -o $@

vpdump: vpdump.c $(SRC)/video/rsx/xf_program.c $(SRC)/core/gx/gx_state.c $(SRC)/core/gx/bp.c $(SRC)/core/gx/gx_vertex.c $(SRC)/video/rsx/gx_features.c $(SRC)/common/log.c
	$(CC) -std=gnu11 -O0 -g -DVP_TRACE -I$(SRC) -w $^ -o $@

clean:
	rm -f tevdump vpdump
