CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++14
LDFLAGS = -lws2_32
SRCDIR = src
TESTDIR = tests

all: server.exe client.exe test_oversized.exe test_concurrent.exe test_commands.exe test_hashtable.exe test_avl.exe

hashtable.o: $(SRCDIR)/hashtable.cpp $(SRCDIR)/hashtable.h
	$(CXX) $(CXXFLAGS) -c -o $@ $(SRCDIR)/hashtable.cpp

avl.o: $(SRCDIR)/avl.cpp $(SRCDIR)/avl.h
	$(CXX) $(CXXFLAGS) -c -o $@ $(SRCDIR)/avl.cpp

server.exe: $(SRCDIR)/server.cpp $(SRCDIR)/hashtable.h $(SRCDIR)/serialization.h $(SRCDIR)/avl.h hashtable.o avl.o
	$(CXX) $(CXXFLAGS) -o $@ $(SRCDIR)/server.cpp hashtable.o avl.o $(LDFLAGS)

client.exe: $(SRCDIR)/client.cpp $(SRCDIR)/serialization.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCDIR)/client.cpp $(LDFLAGS)

test_oversized.exe: $(TESTDIR)/test_oversized.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_concurrent.exe: $(TESTDIR)/test_concurrent.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_commands.exe: $(TESTDIR)/test_commands.cpp $(SRCDIR)/serialization.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_commands.cpp $(LDFLAGS)

test_hashtable.exe: $(TESTDIR)/test_hashtable.cpp $(SRCDIR)/hashtable.h hashtable.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_hashtable.cpp hashtable.o

test_avl.exe: $(TESTDIR)/test_avl.cpp $(SRCDIR)/avl.h avl.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_avl.cpp avl.o

clean:
	-del /Q *.exe *.o 2>NUL

.PHONY: all clean

