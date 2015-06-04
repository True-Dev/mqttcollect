PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man/man1

CFLAGS= -Wall -Werror
LDFLAGS=-lmosquitto # -lssl -lcrypto -lrt

all: mqttcollect

mqttcollect: mqttcollect.c uthash.h json.o utstring.h ini.o
	$(CC) $(CFLAGS) -o mqttcollect mqttcollect.c json.o ini.o $(LDFLAGS)

json.o: json.c json.h
ini.o: ini.c ini.h

install: mqttcollect
	install -m 755 mqttcollect $(BINDIR)/
	install -m 644 mqttcollect.1 $(MANDIR)/

clean:
	rm -f *.o

clobber: clean
	rm -f mqttcollect

doc: README.md mqttcollect.1

README.md: mqttcollect.pandoc
	pandoc -w markdown mqttcollect.pandoc -o README.md

mqttcollect.1: mqttcollect.pandoc
	pandoc -s -w man mqttcollect.pandoc -o mqttcollect.1
