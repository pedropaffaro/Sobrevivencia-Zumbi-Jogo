all:
	gcc zombies.c -o zombies -lpthread
run:
	./zombies
clean:
	rm -f zombies
	clear