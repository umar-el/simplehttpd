# makefile
# SimpleHTTPd - Stage 3 tests (run against the Stage 2 sources)

SRC      = ../Stage2/src
CFLAGS   = -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all -I$(SRC)
UNIT_SRC = tests/test_units.c $(SRC)/http.c $(SRC)/files.c $(SRC)/log.c

test: unit integration

unit: bin/test_units
	./bin/test_units

bin/test_units: $(UNIT_SRC) $(SRC)/simplehttpd.h
	mkdir -p bin
	gcc $(CFLAGS) $(UNIT_SRC) -o bin/test_units

integration:
	./tests/integration.sh

.PHONY: test unit integration
