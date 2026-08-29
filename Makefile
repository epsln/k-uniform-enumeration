CXX := g++
CXXFLAGS := -O3 -std=c++17 -Wall -Wextra -march=native -fno-strict-aliasing -pthread -Isrc -MMD -MP
LDLIBS := -lzstd
TARGET := eusolver

SRCDIR := src
SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(SRCDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEPS)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) eusolver_dbg

debug:
	$(CXX) -g -O0 -std=c++17 -Wall -Wextra -pthread -Isrc -o eusolver_dbg $(SRCS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET) --max-polygons 4 --output solutions_cpp

.PHONY: all clean run debug
