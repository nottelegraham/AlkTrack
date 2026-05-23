CXX      ?= clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I.
TARGET   := alktrack_emu
SRC      := main.cpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC) Arduino.h EEPROM.h ../alkalinity_monitor_v2_1.ino
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) eeprom.bin
