import socket
import argparse

DEFAULT_PORT = 1234
#https://docs.python.org/2/howto/sockets.html

parser = argparse.ArgumentParser(description='UDP tool')
parser.add_argument('--port', type=int, default=DEFAULT_PORT,
                    help="Port to send to")
parser.add_argument('--addr', type=str, required=True,
                    help="Addr to send to E.X. "
                    "'fd00:ff1:ce0b:a5e0:fec2:3d00:4:ea8c'")
parser.add_argument('--data', type=str, required=True,
                    help="Message to send")
parser.add_argument('--ipv6', action="store_true",
                    help="Send packet over ipv6")
args = parser.parse_args()

addr = args.addr
port = args.port
data = args.data
ipv6 = args.ipv6

socket_type = socket.AF_INET6 if ipv6 else socket.AF_INET
sock = socket.socket(socket_type, socket.SOCK_DGRAM)
sock.sendto(data, (addr, port))
print("Message sent")
