CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra

fancontrol: fancontrol.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

# Fully static, stripped binary for releases — runs on any x86_64 Linux regardless of glibc age
.PHONY: release
release: fancontrol.cpp
	$(CXX) $(CXXFLAGS) -static -s -o fancontrol $<

tests: tests.cpp fancontrol.cpp
	$(CXX) $(CXXFLAGS) -o $@ tests.cpp

.PHONY: test install clean
test: tests
	./tests

# Installs the systemd service for the binary and config in this directory.
# Needs root: sudo make install
install: fancontrol
	./install_service.sh

clean:
	rm -f fancontrol tests
