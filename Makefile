CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra

fancontrol: fancontrol.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

tests: tests.cpp fancontrol.cpp
	$(CXX) $(CXXFLAGS) -o $@ tests.cpp

.PHONY: test install clean
test: tests
	./tests

# Installs binary, config, and systemd service. Needs root: sudo make install
install: fancontrol
	./install_service.sh

clean:
	rm -f fancontrol tests
