lazyenv: src/*.c
	clang src/*.c -o build/lazyenv -lwininet -lshell32 -mwindows