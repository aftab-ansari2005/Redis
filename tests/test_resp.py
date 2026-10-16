#!/usr/bin/env python3
import socket
import time
import sys

HOST = "127.0.0.1"
PORT = 1234

def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    return s

def test_basic_commands():
    print("[1/6] Testing basic inline & RESP commands...")
    s = connect()

    # Inline PING
    s.sendall(b"PING\r\n")
    res = s.recv(1024)
    assert res == b"+PONG\r\n", f"Expected +PONG\\r\\n, got {res}"

    # RESP PING with arg
    s.sendall(b"*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n")
    res = s.recv(1024)
    assert res == b"$5\r\nhello\r\n", f"Expected $5\\r\\nhello\\r\\n, got {res}"

    # Inline ECHO
    s.sendall(b"ECHO byo-redis\r\n")
    res = s.recv(1024)
    assert res == b"$9\r\nbyo-redis\r\n", f"Expected $9\\r\\nbyo-redis\\r\\n, got {res}"

    # SET
    s.sendall(b"*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n")
    res = s.recv(1024)
    assert res == b"+OK\r\n", f"Expected +OK\\r\\n, got {res}"

    # GET existing
    s.sendall(b"*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n")
    res = s.recv(1024)
    assert res == b"$3\r\nbar\r\n", f"Expected $3\\r\\nbar\\r\\n, got {res}"

    # GET non-existing (null bulk)
    s.sendall(b"*2\r\n$3\r\nGET\r\n$7\r\nmissing\r\n")
    res = s.recv(1024)
    assert res == b"$-1\r\n", f"Expected $-1\\r\\n, got {res}"

    # DEL
    s.sendall(b"*2\r\n$3\r\nDEL\r\n$3\r\nfoo\r\n")
    res = s.recv(1024)
    assert res == b":1\r\n", f"Expected :1\\r\\n, got {res}"

    # DEL already deleted
    s.sendall(b"*2\r\n$3\r\nDEL\r\n$3\r\nfoo\r\n")
    res = s.recv(1024)
    assert res == b":0\r\n", f"Expected :0\\r\\n, got {res}"

    s.close()
    print("  -> Passed!")

def test_redis_cli_handshake():
    print("[2/6] Testing modern redis-cli connect handshake (COMMAND, HELLO, CLIENT)...")
    s = connect()

    # HELLO 3 negotiation fallback
    s.sendall(b"*2\r\n$5\r\nHELLO\r\n$1\r\n3\r\n")
    res = s.recv(1024)
    assert res.startswith(b"-ERR"), f"Expected -ERR, got {res}"

    # COMMAND (docs)
    s.sendall(b"*1\r\n$7\r\nCOMMAND\r\n")
    res = s.recv(1024)
    assert res == b"*0\r\n", f"Expected *0\\r\\n, got {res}"

    # CLIENT SETINFO
    s.sendall(b"*3\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$7\r\nlib-ver\r\n")
    res = s.recv(1024)
    assert res == b"+OK\r\n", f"Expected +OK\\r\\n, got {res}"

    s.close()
    print("  -> Passed!")

def test_split_packets():
    print("[3/6] Testing incremental parsing (command split across socket writes)...")
    s = connect()

    # Split a SET command across multiple writes with pauses
    part1 = b"*3\r\n$3\r\nSET\r\n$9\r\nsplit"
    part2 = b"_key\r\n$11\r\nsplit_value\r\n"

    s.sendall(part1)
    time.sleep(0.05) # simulate network jitter / fragmented TCP window
    s.sendall(part2)

    res = s.recv(1024)
    assert res == b"+OK\r\n", f"Expected +OK\\r\\n, got {res}"

    # Verify key was properly stored
    s.sendall(b"*2\r\n$3\r\nGET\r\n$9\r\nsplit_key\r\n")
    res = s.recv(1024)
    assert res == b"$11\r\nsplit_value\r\n", f"Expected $11\\r\\nsplit_value\\r\\n, got {res}"

    s.close()
    print("  -> Passed!")

def test_pipelining():
    print("[4/6] Testing pipelined batch of 1,000 requests in single write...")
    s = connect()

    N = 1000
    batch = []
    for i in range(N):
        k = f"k_{i}"
        v = f"v_{i}"
        cmd = f"*3\r\n$3\r\nSET\r\n${len(k)}\r\n{k}\r\n${len(v)}\r\n{v}\r\n"
        batch.append(cmd.encode("utf-8"))

    payload = b"".join(batch)
    s.sendall(payload)

    # Read back all 1000 responses
    received = b""
    expected_len = N * len(b"+OK\r\n")
    while len(received) < expected_len:
        chunk = s.recv(65536)
        if not chunk:
            break
        received += chunk

    assert len(received) == expected_len, f"Expected {expected_len} bytes, got {len(received)}"
    assert received == b"+OK\r\n" * N, "Response mismatch in pipeline"

    s.close()
    print(f"  -> Passed! (Successfully processed {N} pipelined SETs)")

def test_safety_limits():
    print("[5/6] Testing safety limits ($99999999999 bounds attack)...")
    s = connect()

    # Send oversized length header
    s.sendall(b"*1\r\n$99999999999\r\nfoo\r\n")
    # Server must reject protocol violation and close connection without crashing
    res = s.recv(1024)
    assert len(res) == 0, f"Expected connection close, got {res}"

    s.close()
    print("  -> Passed! (Server rejected oversized length and closed socket cleanly)")

def test_quit():
    print("[6/6] Testing QUIT command...")
    s = connect()

    s.sendall(b"QUIT\r\n")
    res = s.recv(1024)
    assert res == b"+OK\r\n", f"Expected +OK\\r\\n, got {res}"

    # Server should close connection after sending +OK\r\n
    remaining = s.recv(1024)
    assert remaining == b"", f"Expected EOF after QUIT, got {remaining}"

    s.close()
    print("  -> Passed!")

def main():
    print("==================================================")
    print(" Antigravity Redis — RESP2 Protocol Test Suite    ")
    print("==================================================")
    test_basic_commands()
    test_redis_cli_handshake()
    test_split_packets()
    test_pipelining()
    test_safety_limits()
    test_quit()
    print("==================================================")
    print(" All tests passed successfully!                   ")
    print("==================================================")

if __name__ == "__main__":
    main()
