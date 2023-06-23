main: main.cpp simdjson.o
	g++ -o main main.cpp simdjson.o -O3 -Wall -Wextra -std=c++17 -Iperformancecounters -I. -fno-exceptions

simdjson.o: simdjson.cpp  simdjson.h
	g++ -c  simdjson.cpp -O3 -Wall -Wextra -std=c++17 -fno-exceptions

clean:
	rm -f main simdjson.o