# Native unit-test build for the portable slide-whistle core.
# Runs anywhere with a C++17 compiler — no PlatformIO or ESP32 toolchain needed.
#
#   make        # build + run all tests
#   make build  # build only

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wshadow -O1 -I.
SRC      := main.cpp $(wildcard test_*.cpp)
BIN      := run_tests

# The ESP32 backend sinks live behind #if defined(ARDUINO), so they build in a
# SEPARATE binary with a minimal Arduino stub — this exercises the REAL
# EspMotionSink (review #8 §1), not just a fake IMotionSink.
ESPSINK_SRC := espsink/test_espmotionsink.cpp
ESPSINK_BIN := run_espsink

.PHONY: test build clean
test: build
	./$(BIN)
	./$(ESPSINK_BIN)

build: $(BIN) $(ESPSINK_BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

$(ESPSINK_BIN): $(ESPSINK_SRC)
	$(CXX) -std=c++17 -Wall -Wextra -Iespsink $(ESPSINK_SRC) -o $(ESPSINK_BIN)

clean:
	rm -f $(BIN) $(ESPSINK_BIN)
