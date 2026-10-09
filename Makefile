# qnx_hackathon - QNX 8.0, Raspberry Pi 4 (aarch64)
#
#   car/      abs.c decoder.c (nav.c later)
#   common/   actors.c actors.h taskmgr.c tm_proto.h
#   tools/    panel.c dashboard.py
#
#   build/obj/   object files (.o)
#   build/       finished programs
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
BINS    = $(BIN)/taskmgr $(BIN)/abs $(BIN)/decoder $(BIN)/panel $(BIN)/nav
# nav: when car/nav.c exists, add $(BIN)/nav to BINS and uncomment its two rules below

# headers every actor depends on (a change rebuilds them all)
ACT_H   = common/actors.h common/tm_proto.h

all: $(BINS)

# ---- link: objects -> programs (actors.o is shared, compiled once) ----
$(BIN)/taskmgr: $(OBJ)/taskmgr.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/taskmgr.o

$(BIN)/abs: $(OBJ)/abs.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/abs.o $(OBJ)/actors.o

$(BIN)/decoder: $(OBJ)/decoder.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/decoder.o $(OBJ)/actors.o
	
$(BIN)/nav: $(OBJ)/nav.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/nav.o $(OBJ)/actors.o

$(BIN)/panel: $(OBJ)/panel.o $(OBJ)/actors.o
	$(CC) $(LDFLAGS) -o $@ $(OBJ)/panel.o $(OBJ)/actors.o

#$(BIN)/nav: $(OBJ)/nav.o $(OBJ)/actors.o
#	$(CC) $(LDFLAGS) -o $@ $(OBJ)/nav.o $(OBJ)/actors.o

# ---- compile: sources -> objects ----
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

$(OBJ)/panel.o: tools/panel.c $(ACT_H) | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ tools/panel.c

#$(OBJ)/nav.o: car/nav.c $(ACT_H) | $(OBJ)
#	$(CC) $(CFLAGS) -c -o $@ car/nav.c

# object folder, created once (also safe with make -j)
$(OBJ):
	mkdir -p $(OBJ)

deploy: all
	scp -o MACs=hmac-sha2-256 $(BINS) tools/dashboard.py $(PI):/tmp/

clean:
	rm -f $(BIN)/taskmgr $(BIN)/abs $(BIN)/decoder $(BIN)/panel $(BIN)/nav $(OBJ)/*.o

.PHONY: all deploy clean