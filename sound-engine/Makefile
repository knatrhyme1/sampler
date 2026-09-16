CC = gcc
CFLAGS = -Wall -O2
LDLIBS = -lm

sound-engine: main.c
	$(CC) $(CFLAGS) -o sound-engine main.c $(LDLIBS)

.PHONY: test
test: sound-engine
	./sound-engine test_tone.wav

.PHONY: clean
clean:
	rm -f sound-engine test_tone.wav
