#!/usr/bin/env python3
import socket
import time

HOST = "127.0.0.1"
PORT = 1234
TOTAL = 100000
PIPELINE = 1000

def bench_set():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    
    # Pre-build 1000-cmd pipeline packet
    cmds = []
    for i in range(PIPELINE):
        cmds.append(f"*3\r\n$3\r\nSET\r\n$4\r\nk{i:03d}\r\n$4\r\nv{i:03d}\r\n".encode())
    batch = b"".join(cmds)
    expected_bytes = PIPELINE * len(b"+OK\r\n")
    
    start = time.perf_counter()
    sent = 0
    while sent < TOTAL:
        s.sendall(batch)
        recv_bytes = 0
        while recv_bytes < expected_bytes:
            chunk = s.recv(65536)
            if not chunk:
                break
            recv_bytes += len(chunk)
        sent += PIPELINE
    elapsed = time.perf_counter() - start
    s.close()
    ops_sec = TOTAL / elapsed
    print(f"SET: {ops_sec:,.2f} requests per second ({TOTAL} requests in {elapsed:.3f}s)")

def bench_get():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    
    cmds = []
    for i in range(PIPELINE):
        cmds.append(f"*2\r\n$3\r\nGET\r\n$4\r\nk{i:03d}\r\n".encode())
    batch = b"".join(cmds)
    # v000 is bulk string: $4\r\nv000\r\n (11 bytes)
    expected_bytes = PIPELINE * len(b"$4\r\nv000\r\n")
    
    start = time.perf_counter()
    sent = 0
    while sent < TOTAL:
        s.sendall(batch)
        recv_bytes = 0
        while recv_bytes < expected_bytes:
            chunk = s.recv(65536)
            if not chunk:
                break
            recv_bytes += len(chunk)
        sent += PIPELINE
    elapsed = time.perf_counter() - start
    s.close()
    ops_sec = TOTAL / elapsed
    print(f"GET: {ops_sec:,.2f} requests per second ({TOTAL} requests in {elapsed:.3f}s)")

if __name__ == "__main__":
    print(f"--- Running Micro-benchmark (100,000 requests, pipeline batch {PIPELINE}) ---")
    bench_set()
    bench_get()
