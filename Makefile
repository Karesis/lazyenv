lazyenv: src/*.c
	clang src/*.c -o lazyenv -lwininet -lshell32 -mwindows