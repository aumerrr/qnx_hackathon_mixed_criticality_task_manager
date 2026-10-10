# qnx_hackathon - build for QNX 8.0 on Raspberry Pi 4 (aarch64, qcc)
#
#   car/      actors: abs.c airbag.c decoder.c nav.c ui.c
#   common/   shared: actors.c actors.h (actor library), taskmgr.c, tm_proto.h
#   tools/    panel.c (LED panel), dashboard.py (not compiled)
#
#   build/obj/   object files      build/   finished programs
#
#   make          build all programs
#   make deploy   copy the programs + dashboard.py to the Pi (/tmp)
#   make clean    delete programs and objects

CC      = qcc
TARGET  = -Vgcc_ntoaarch64le
CFLAGS  = $(TARGET) -Wall -g -O2 -std=gnu11 -Icommon
LDFLAGS = $(TARGET)
PI      = qnxuser@10.0.0.1

BIN     = build
OBJ     = build/obj
BINS    = $(BIN)/taskmgr $(BIN)/abs $(BIN)/airbag $(BIN)/decoder $(BIN)/nav $(BIN)/ui $(BIN)/panel

# Headers every actor includes: changing one rebuilds every actor, so all
# programs always agree on the tm_proto.h message layout.
ACT_H   = common/actors.h common/tm_proto.h

all: $(BINS)

# ---- link: objects -> programs (actors.o is compiled once, linked into each actor) ----
$(BIN)/taskmgr: $(OBJ)/taskmgr.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/taskmgr.o

$(BIN)/abs: $(OBJ)/abs.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/abs.o $(OBJ)/actors.o

$(BIN)/decoder: $(OBJ)/decoder.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/decoder.o $(OBJ)/actors.o
	
$(BIN)/nav: $(OBJ)/nav.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/nav.o $(OBJ)/actors.o

$(BIN)/airbag: $(OBJ)/airbag.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/airbag.o $(OBJ)/actors.o

$(BIN)/ui: $(OBJ)/ui.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/ui.o $(OBJ)/actors.o

$(BIN)/panel: $(OBJ)/panel.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/panel.o $(OBJ)/actors.o

# ---- compile: sources -> objects (-Icommon finds the shared headers) ----
$(OBJ)/taskmgr.o: common/taskmgr.c common/tm_proto.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ common/taskmgr.c

$(OBJ)/actors.o: common/actors.c common/actors.h | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ common/actors.c

$(OBJ)/abs.o: car/abs.c $(ACT_H) | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ car/abs.c

$(OBJ)/decoder.o: car/decoder.c $(ACT_H) | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ car/decoder.c
	
$(OBJ)/nav.o: car/nav.c $(ACT_H) | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ car/nav.c

$(OBJ)/airbag.o: car/airbag.c $(ACT_H) | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ car/airbag.c

$(OBJ)/ui.o: car/ui.c $(ACT_H) | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ car/ui.c

$(OBJ)/panel.o: tools/panel.c $(ACT_H) | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ tools/panel.c

# Object folder; order-only prerequisite (|), so it is created once and is safe with make -j.
$(OBJ):
	mkdir -p $(OBJ)

deploy: all
	scp -o MACs=hmac-sha2-256 $(BINS) tools/dashboard.py $(PI):/tmp/

clean:
	rm -f $(BINS) $(OBJ)/*.o

.PHONY: all deploy clean