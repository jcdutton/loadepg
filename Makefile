loadepg: loadepg.o
	gcc -g -oloadepg loadepg.o

loadepg.o: loadepg.c
	gcc -g -c -oloadepg.o loadepg.c

clean: 
	rm *.o
	rm loadepg
