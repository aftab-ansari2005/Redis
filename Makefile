CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++14
LDFLAGS = -lws2_32
SRCDIR = src
TESTDIR = tests

all: server.exe client.exe test_oversized.exe test_concurrent.exe test_commands.exe test_hashtable.exe test_avl.exe test_zset.exe test_dlist.exe test_timers.exe test_heap.exe test_ttl.exe test_thread_pool.exe test_lazyfree.exe

hashtable.o: $(SRCDIR)/hashtable.cpp $(SRCDIR)/hashtable.h
	$(CXX) $(CXXFLAGS) -c -o $@ $(SRCDIR)/hashtable.cpp

avl.o: $(SRCDIR)/avl.cpp $(SRCDIR)/avl.h
	$(CXX) $(CXXFLAGS) -c -o $@ $(SRCDIR)/avl.cpp

zset.o: $(SRCDIR)/zset.cpp $(SRCDIR)/zset.h $(SRCDIR)/hashtable.h $(SRCDIR)/avl.h
	$(CXX) $(CXXFLAGS) -c -o $@ $(SRCDIR)/zset.cpp

heap.o: $(SRCDIR)/heap.cpp $(SRCDIR)/heap.h
	$(CXX) $(CXXFLAGS) -c -o $@ $(SRCDIR)/heap.cpp

thread_pool.o: $(SRCDIR)/thread_pool.cpp $(SRCDIR)/thread_pool.h
	$(CXX) $(CXXFLAGS) -c -o $@ $(SRCDIR)/thread_pool.cpp

server.exe: $(SRCDIR)/server.cpp $(SRCDIR)/hashtable.h $(SRCDIR)/resp.h $(SRCDIR)/avl.h $(SRCDIR)/zset.h $(SRCDIR)/dlist.h $(SRCDIR)/heap.h $(SRCDIR)/thread_pool.h hashtable.o avl.o zset.o heap.o thread_pool.o
	$(CXX) $(CXXFLAGS) -o $@ $(SRCDIR)/server.cpp hashtable.o avl.o zset.o heap.o thread_pool.o $(LDFLAGS)

client.exe: $(SRCDIR)/client.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCDIR)/client.cpp $(LDFLAGS)

test_oversized.exe: $(TESTDIR)/test_oversized.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_concurrent.exe: $(TESTDIR)/test_concurrent.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_commands.exe: $(TESTDIR)/test_commands.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_commands.cpp $(LDFLAGS)

test_hashtable.exe: $(TESTDIR)/test_hashtable.cpp $(SRCDIR)/hashtable.h hashtable.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_hashtable.cpp hashtable.o

test_avl.exe: $(TESTDIR)/test_avl.cpp $(SRCDIR)/avl.h avl.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_avl.cpp avl.o

test_zset.exe: $(TESTDIR)/test_zset.cpp $(SRCDIR)/zset.h zset.o hashtable.o avl.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_zset.cpp zset.o hashtable.o avl.o

test_dlist.exe: $(TESTDIR)/test_dlist.cpp $(SRCDIR)/dlist.h $(SRCDIR)/hashtable.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_dlist.cpp

test_timers.exe: $(TESTDIR)/test_timers.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_timers.cpp $(LDFLAGS)

test_heap.exe: $(TESTDIR)/test_heap.cpp $(SRCDIR)/heap.h heap.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_heap.cpp heap.o

test_ttl.exe: $(TESTDIR)/test_ttl.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_ttl.cpp $(LDFLAGS)

test_thread_pool.exe: $(TESTDIR)/test_thread_pool.cpp $(SRCDIR)/thread_pool.h thread_pool.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_thread_pool.cpp thread_pool.o

test_lazyfree.exe: $(TESTDIR)/test_lazyfree.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_lazyfree.cpp $(LDFLAGS)

clean:
	-del /Q *.exe *.o 2>NUL

.PHONY: all clean

