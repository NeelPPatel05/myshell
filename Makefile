CC  = gcc
CFLAGS = -Wall -Werror -g
SANITIZE_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

all: mysh

mysh: mysh.o
	$(CC) $(CFLAGS) -o $@ $^

mysh.o: mysh.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Build a sanitizer-enabled version of the shell
san: clean
	$(MAKE) CFLAGS="$(CFLAGS) $(SANITIZE_FLAGS)" mysh

clean:
	rm -f mysh mysh.o
