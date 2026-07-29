# Native unit-test build for the portable slide-whistle core.
# Runs anywhere with a C++17 compiler — no PlatformIO or ESP32 toolchain needed.
#
#   make        # build + run all tests
#   make build  # build only

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wshadow -O1 -I.
SRC      := main.cpp $(wildcard test_*.cpp)
BIN      := run_tests

.PHONY: test build clean
test: build
	./$(BIN)

build:
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

clean:
	rm -f $(BIN)
