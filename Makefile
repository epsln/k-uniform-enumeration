CXX := g++
CXXFLAGS := -O3 -std=c++17 -Wall -Wextra -march=native -fno-strict-aliasing -pthread -Isrc
LDLIBS := -lzstd -lsqlite3
TARGET := eusolver
SAFETY_TEST := tests/high_k_safety

SRCDIR := src
SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) eusolver_dbg $(SAFETY_TEST)

debug:
	$(CXX) -g -O0 -std=c++17 -Wall -Wextra -pthread -Isrc -o eusolver_dbg $(SRCS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET) --max-polygons 4 --output solutions_cpp

# TEST_MAX_K defaults to the quick k=1 suite. Set BASELINE to also compare the
# exact canonical solution set against another executable.
$(SAFETY_TEST): tests/high_k_safety.cpp $(filter-out $(SRCDIR)/main.o,$(OBJS))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

test: $(TARGET) $(SAFETY_TEST)
	./$(SAFETY_TEST)
	python3 -m unittest discover -s tests -v
	python3 scripts/regression.py --candidate "./$(TARGET)" --max-k $(or $(TEST_MAX_K),1) $(if $(BASELINE),--baseline "$(BASELINE)",)

# Benchmarking deliberately has no dependency on $(TARGET): it must not rebuild
# either executable while comparing two supplied binaries.
benchmark:
	@test -n "$(BASELINE)" || { printf '%s\n' 'usage: make benchmark BASELINE=/path/to/baseline [CANDIDATE=./eusolver]'; exit 2; }
	python3 scripts/benchmark.py --baseline "$(BASELINE)" --candidate "$(or $(CANDIDATE),./$(TARGET))" --max-k $(or $(BENCHMARK_MAX_K),2) --repeats $(or $(BENCHMARK_REPEATS),3) $(if $(BENCHMARK_MAX_SLOWDOWN),--max-slowdown $(BENCHMARK_MAX_SLOWDOWN),)

.PHONY: all clean run debug test benchmark
