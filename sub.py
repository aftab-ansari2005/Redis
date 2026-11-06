# sub.py — Standalone Pub/Sub subscriber test script
import socket
import sys

port = 1234
channel = sys.argv[1] if len(sys.argv) > 1 else "news"

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    s.connect(("127.0.0.1", port))
except Exception as e:
    print(f"Could not connect to Redis server on 127.0.0.1:{port}: {e}")
    sys.exit(1)

# Format RESP2 command: SUBSCRIBE <channel>
cmd = f"*2\r\n$9\r\nSUBSCRIBE\r\n${len(channel)}\r\n{channel}\r\n".encode()
s.sendall(cmd)

print(f"Reading messages on channel '{channel}'... (Press Ctrl-C to quit)\n")

try:
    while True:
        data = s.recv(4096)
        if not data:
            print("Server closed connection.")
            break
        print(data.decode(errors="replace").strip())
        print("-" * 35)
except KeyboardInterrupt:
    print("\nExiting subscriber.")
finally:
    s.close()

