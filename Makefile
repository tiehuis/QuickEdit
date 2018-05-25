CC     := clang
CFLAGS := -O3 -std=c99 -pedantic -Wall -Wextra

qe: qe.c
	$(CC) $(CFLAGS) $^ -o $@

gen: gen.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f qe gen *.txt

.PHONY: clean
