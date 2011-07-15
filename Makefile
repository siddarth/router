all: router shaper

router:
	gcc router.c -std=c99 -lpthread -g -D_POSIX_SOURCE -o router

shaper:
	gcc shaper.c -std=c99 -lpthread -g -D_POSIX_SOURCE -o shaper

clean:
	rm -f router shaper
