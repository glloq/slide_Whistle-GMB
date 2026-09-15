# Native unit-test build for the portable slide-whistle core.
# Runs anywhere with a C++17 compiler — no PlatformIO or ESP32 toolchain needed.
#
#   make        # build + run all tests
#   make build  # build only

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wshadow -O1 -I.
SRC      := main.cpp $(wildcard test_*.cpp)
BIN      := run_tests
# All core/platform headers — listed as prerequisites so editing a header
# (the bulk of the code is header-only) actually forces a rebuild.
HDRS     := $(wildcard ../esp32/esp32_slide_whistle/core/*.h \
                       ../esp32/esp32_slide_whistle/core/gmb/*.h \
                       ../esp32/esp32_slide_whistle/core/platform/*.h) \
            $(wildcard *.h)

# The ESP32 backend sinks live behind #if defined(ARDUINO), so they build in a
# SEPARATE binary with a minimal Arduino stub — this exercises the REAL
# EspMotionSink (review #8 §1), not just a fake IMotionSink.
ESPSINK_SRC := espsink/test_espmotionsink.cpp
ESPSINK_BIN := run_espsink

# Regenerates the checked-in GMB descriptor example from the REAL capability
# builder, so the documented example can never drift from what the firmware
# serves (tests/test_gmb_capabilities.cpp compares them byte for byte).
FIXTURE_SRC := tools/gmb_descriptor_dump.cpp
FIXTURE_BIN := gmb_descriptor_dump
FIXTURE     := fixtures/gmb_descriptor_reference.json

# The ESP32-guarded platform layer (MainApp, the GMB platform half, the web
# adapter) cannot RUN natively, but it can be COMPILED against a stub of the
# arduino-esp32 API. That catches a typo in seconds instead of waiting for the
# PlatformIO job, and it covers both arduino-esp32 API generations and both
# board profiles.
STUB_SRC   := ardstub/stub_syntax_check.cpp
STUB_FLAGS := -std=c++17 -Wall -Wextra -Iardstub -fsyntax-only

.PHONY: test build clean fixture platformcheck
test: build platformcheck
	./$(BIN)
	./$(ESPSINK_BIN)

build: $(BIN) $(ESPSINK_BIN)

platformcheck: $(STUB_SRC) $(HDRS)
	@for V in "300 -DESP_ARDUINO_VERSION_MAJOR=3" "200"; do \
	  for B in BOARD_ESP32_WROOM BOARD_ESP32_S3; do \
	    echo "[ SYNTAX ] arduino-esp32 $$V / $$B"; \
	    $(CXX) $(STUB_FLAGS) -DARDUINO=$$V -D$$B $(STUB_SRC) || exit 1; \
	  done; \
	done

fixture: $(FIXTURE_BIN)
	./$(FIXTURE_BIN) $(FIXTURE)

$(FIXTURE_BIN): $(FIXTURE_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) $(FIXTURE_SRC) -o $(FIXTURE_BIN)

$(BIN): $(SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

$(ESPSINK_BIN): $(ESPSINK_SRC) $(HDRS) espsink/Arduino.h
	$(CXX) -std=c++17 -Wall -Wextra -Iespsink $(ESPSINK_SRC) -o $(ESPSINK_BIN)

clean:
	rm -f $(BIN) $(ESPSINK_BIN) $(FIXTURE_BIN)
