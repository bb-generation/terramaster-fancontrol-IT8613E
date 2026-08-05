CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra

fancontrol: fancontrol.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

tests: tests.cpp fancontrol.cpp
	$(CXX) $(CXXFLAGS) -o $@ tests.cpp

.PHONY: test clean
test: tests
	./tests

clean:
	rm -f fancontrol tests
