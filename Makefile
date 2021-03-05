
BLUETOOTH=no
MQTT=yes

PROG=$(shell basename $(shell pwd))
MYBMM_SRC=../mybmm
CELLMONS=$(shell cat $(MYBMM_SRC)/Makefile | grep ^CELLMONS= | awk -F= '{ print $$2 }')
TRANSPORTS=$(shell cat $(MYBMM_SRC)/Makefile | grep ^TRANSPORTS= | head -1 | awk -F= '{ print $$2 }')
ifneq ($(BLUETOOTH),yes)
_TMPVAR := $(TRANSPORTS)
TRANSPORTS = $(filter-out bt.c, $(_TMPVAR))
endif
UTILS=$(shell cat $(MYBMM_SRC)/Makefile | grep ^UTILS= | awk -F= '{ print $$2 }')
SRCS=main.c display.c module.c pack.c mqtt.c parson.c $(CELLMONS) $(TRANSPORTS) $(UTILS)
CFLAGS=-I$(MYBMM_SRC) -DTHREAD_SAFE
#CFLAGS+=-Wall -O2 -pipe
CFLAGS+=-Wall -g -DDEBUG
LIBS+=-ldl -lpthread
ifeq ($(MQTT),yes)
CFLAGS+=-DMQTT
LIBS+=-lpaho-mqtt3c
endif
ifeq ($(BLUETOOTH),yes)
CFLAGS+=-DBLUETOOTH
LIBS+=-lgattlib -lglib-2.0 -lpthread
endif
LDFLAGS+=-rdynamic
OBJS=$(SRCS:.c=.o)

vpath %.c $(MYBMM_SRC)

.PHONY: all
all: $(PROG)

$(PROG): $(OBJS) $(DEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(PROG) $(OBJS) $(LIBS)

$(OBJS): Makefile

debug: $(PROG)
	gdb ./$(PROG)

include $(MYBMM_SRC)/Makefile.dep

#BINDIR=$(shell if "$$(id -u)" = "0"; then echo "-o bin -g bin /usr/local/bin/"; else echo ~/bin/; fi)
#ETCDIR=$(shell if "$$(id -u)" = "0"; then echo /usr/local/etc/; else echo ~/etc/; fi)

install: $(PROG)
	sudo install -m 755 -o bin -g bin $(PROG) /usr/local/bin/$(PROG)

clean:
	rm -rf $(PROG) $(OBJS) $(CLEANFILES)

push: clean
	git add -A .
	git commit -m refresh
	git push

pull: clean
	git reset --hard
	git pull
