CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++14
LDFLAGS = -lws2_32
SRCDIR = src
TESTDIR = tests

all: server.exe client.exe test_oversized.exe test_concurrent.exe test_commands.exe test_hashtable.exe

hashtable.o: $(SRCDIR)/hashtable.cpp $(SRCDIR)/hashtable.h
	$(CXX) $(CXXFLAGS) -c -o $@ $(SRCDIR)/hashtable.cpp

server.exe: $(SRCDIR)/server.cpp $(SRCDIR)/hashtable.h hashtable.o
	$(CXX) $(CXXFLAGS) -o $@ $(SRCDIR)/server.cpp hashtable.o $(LDFLAGS)

client.exe: $(SRCDIR)/client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_oversized.exe: $(TESTDIR)/test_oversized.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_concurrent.exe: $(TESTDIR)/test_concurrent.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_commands.exe: $(TESTDIR)/test_commands.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_hashtable.exe: $(TESTDIR)/test_hashtable.cpp $(SRCDIR)/hashtable.h hashtable.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_hashtable.cpp hashtable.o

clean:
	-del /Q *.exe *.o 2>NUL

.PHONY: all clean

