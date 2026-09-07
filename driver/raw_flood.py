import socket, struct, sys

iface = "vnic0"
count = int(sys.argv[1]) if len(sys.argv) > 1 else 5000

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind((iface, 0))

dst = bytes.fromhex("020000000002")
src = bytes.fromhex("020000000001")
ethertype = struct.pack("!H", 0x0800)
payload = b"\x00" * 46
frame = dst + src + ethertype + payload

for _ in range(count):
    s.send(frame)
