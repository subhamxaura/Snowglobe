"""unix_sockets/sock.py — bind/connect/sendto over AF_UNIX, all deterministic."""
import os
import socket
import sys

d = sys.argv[1]

# Filesystem path: stream listener + connector.
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(os.path.join(d, "sock"))
srv.listen(1)
cli = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
cli.connect(os.path.join(d, "sock"))
conn, _ = srv.accept()
conn.sendall(b"ping")
cli.close()
conn.close()
srv.close()

# Fixed abstract name: stream listener + connector.
srv2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv2.bind("\0sg-snowglobe-abstract")
srv2.listen(1)
cli2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
cli2.connect("\0sg-snowglobe-abstract")
cli2.close()
srv2.close()

# Datagrams with an explicit destination address.
a = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
b = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
a.bind(os.path.join(d, "dg-a"))
b.bind(os.path.join(d, "dg-b"))
a.sendto(b"hi", os.path.join(d, "dg-b"))
a.close()
b.close()
