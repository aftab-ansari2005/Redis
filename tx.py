# tx.py — Standalone Transaction test script
import socket

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    s.connect(("127.0.0.1", 1234))
except Exception as e:
    print(f"Could not connect to Redis server: {e}")
    exit(1)

# Send MULTI, SET, GET, EXEC within the same socket connection
pipeline = (
    "*1\r\n$5\r\nMULTI\r\n"
    "*3\r\n$3\r\nSET\r\n$2\r\nt1\r\n$10\r\ntransacted\r\n"
    "*2\r\n$3\r\nGET\r\n$2\r\nt1\r\n"
    "*1\r\n$4\r\nEXEC\r\n"
).encode()

s.sendall(pipeline)
response = s.recv(4096).decode(errors="replace")
print("Transaction Response from Server:")
print(response)
s.close()

