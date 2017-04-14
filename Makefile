CC     := clang
CFLAGS := -O2 -std=c99 -pedantic -Wall -Wextra

qe: qe.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm qe

.PHONY: clean
