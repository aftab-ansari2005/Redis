CXX ?= g++
CXXFLAGS = -Wall -Wextra -std=c++14 -O2
SRCDIR = src
TESTDIR = tests

ifeq ($(OS),Windows_NT)
    LDFLAGS = -lws2_32
    EXE = .exe
    RM = del /Q *.exe *.o 2>NUL
else
    LDFLAGS = -pthread
    EXE =
    RM = rm -f *.exe *.o $(BINARIES)
endif

OBJS = hashtable.o avl.o zset.o heap.o thread_pool.o

BINARIES = server$(EXE) client$(EXE) \
           test_hashtable$(EXE) test_avl$(EXE) test_zset$(EXE) \
           test_heap$(EXE) test_dlist$(EXE) test_thread_pool$(EXE) \
           test_oversized$(EXE) test_concurrent$(EXE) test_commands$(EXE) \
           test_timers$(EXE) test_ttl$(EXE) test_lazyfree$(EXE) \
           test_aof$(EXE) test_transactions$(EXE) test_pubsub$(EXE)

all: $(BINARIES)

# Aliases for explicit .exe target names on non-Windows platforms
ifneq ($(EXE),.exe)
server.exe: server
client.exe: client
test_hashtable.exe: test_hashtable
test_avl.exe: test_avl
test_zset.exe: test_zset
test_heap.exe: test_heap
test_dlist.exe: test_dlist
test_thread_pool.exe: test_thread_pool
test_oversized.exe: test_oversized
test_concurrent.exe: test_concurrent
test_commands.exe: test_commands
test_timers.exe: test_timers
test_ttl.exe: test_ttl
test_lazyfree.exe: test_lazyfree
test_aof.exe: test_aof
test_transactions.exe: test_transactions
test_pubsub.exe: test_pubsub
endif

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

server$(EXE): $(SRCDIR)/server.cpp $(SRCDIR)/hashtable.h $(SRCDIR)/resp.h $(SRCDIR)/avl.h $(SRCDIR)/zset.h $(SRCDIR)/dlist.h $(SRCDIR)/heap.h $(SRCDIR)/thread_pool.h $(SRCDIR)/aof.h $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCDIR)/server.cpp $(OBJS) $(LDFLAGS)

client$(EXE): $(SRCDIR)/client.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCDIR)/client.cpp $(LDFLAGS)

test_hashtable$(EXE): $(TESTDIR)/test_hashtable.cpp $(SRCDIR)/hashtable.h hashtable.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_hashtable.cpp hashtable.o

test_avl$(EXE): $(TESTDIR)/test_avl.cpp $(SRCDIR)/avl.h avl.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_avl.cpp avl.o

test_zset$(EXE): $(TESTDIR)/test_zset.cpp $(SRCDIR)/zset.h zset.o hashtable.o avl.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_zset.cpp zset.o hashtable.o avl.o

test_heap$(EXE): $(TESTDIR)/test_heap.cpp $(SRCDIR)/heap.h heap.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_heap.cpp heap.o

test_dlist$(EXE): $(TESTDIR)/test_dlist.cpp $(SRCDIR)/dlist.h $(SRCDIR)/hashtable.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_dlist.cpp

test_thread_pool$(EXE): $(TESTDIR)/test_thread_pool.cpp $(SRCDIR)/thread_pool.h thread_pool.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_thread_pool.cpp thread_pool.o $(LDFLAGS)

test_oversized$(EXE): $(TESTDIR)/test_oversized.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_concurrent$(EXE): $(TESTDIR)/test_concurrent.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test_commands$(EXE): $(TESTDIR)/test_commands.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_commands.cpp $(LDFLAGS)

test_timers$(EXE): $(TESTDIR)/test_timers.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_timers.cpp $(LDFLAGS)

test_ttl$(EXE): $(TESTDIR)/test_ttl.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_ttl.cpp $(LDFLAGS)

test_lazyfree$(EXE): $(TESTDIR)/test_lazyfree.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_lazyfree.cpp $(LDFLAGS)

test_aof$(EXE): $(TESTDIR)/test_aof.cpp $(SRCDIR)/resp.h $(SRCDIR)/aof.h thread_pool.o
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_aof.cpp thread_pool.o $(LDFLAGS)

test_transactions$(EXE): $(TESTDIR)/test_transactions.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_transactions.cpp $(LDFLAGS)

test_pubsub$(EXE): $(TESTDIR)/test_pubsub.cpp $(SRCDIR)/resp.h
	$(CXX) $(CXXFLAGS) -o $@ $(TESTDIR)/test_pubsub.cpp $(LDFLAGS)

test: test_hashtable$(EXE) test_avl$(EXE) test_zset$(EXE) test_heap$(EXE) test_dlist$(EXE) test_thread_pool$(EXE)
	./test_hashtable$(EXE)
	./test_avl$(EXE)
	./test_zset$(EXE)
	./test_heap$(EXE)
	./test_dlist$(EXE)
	./test_thread_pool$(EXE)

clean:
	-$(RM)

.PHONY: all clean test
