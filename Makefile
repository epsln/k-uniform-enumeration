build:
	g++ -O2 -std=c++17 -pthread -I./nauty main.cpp ./nauty/nauty.a -o main 
build-test:
	g++ -O2 -std=c++17 -pthread -I./nauty -DSOLVER_TEST_MODE test.cpp ./nauty/nauty.a -o test 
run: 
	./main
run-test: build build-test
	./test
