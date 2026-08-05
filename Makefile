CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra

fancontrol: fancontrol.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

# Fully static binary for releases — runs on any x86_64 Linux regardless of glibc age
.PHONY: release
release: fancontrol.cpp
	$(CXX) $(CXXFLAGS) -static -o fancontrol $<

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
