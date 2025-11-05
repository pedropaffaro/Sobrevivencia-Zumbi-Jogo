all:
	gcc zombies.c -o zombies -lpthread
run:
	./zombies