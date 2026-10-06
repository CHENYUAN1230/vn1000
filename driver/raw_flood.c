/*
 * raw_flood.c - send N raw Ethernet frames at vnic0 as fast as possible.
 *
 * C replacement for raw_flood.py: the busybox nested-VM test image has no
 * Python3, so this does the same thing (AF_PACKET/SOCK_RAW, bind to the
 * interface, blast a fixed frame in a tight loop) as a static binary that
 * can be copied straight into the initramfs.
 *
 * usage: raw_flood [count]   (default 5000, same as the Python version)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netpacket/packet.h>
#include <linux/if_ether.h>

#define IFACE "vnic0"

int main(int argc, char **argv)
{
	long count = argc > 1 ? atol(argv[1]) : 5000;

	int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, IFACE, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		perror("SIOCGIFINDEX");
		close(fd);
		return 1;
	}

	struct sockaddr_ll addr;
	memset(&addr, 0, sizeof(addr));
	addr.sll_family = AF_PACKET;
	addr.sll_ifindex = ifr.ifr_ifindex;
	addr.sll_protocol = htons(ETH_P_ALL);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return 1;
	}

	static const unsigned char dst[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
	static const unsigned char src[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

	unsigned char frame[6 + 6 + 2 + 46];
	memcpy(frame, dst, 6);
	memcpy(frame + 6, src, 6);
	frame[12] = 0x08;
	frame[13] = 0x00;
	memset(frame + 14, 0, 46);

	for (long i = 0; i < count; i++) {
		if (send(fd, frame, sizeof(frame), 0) < 0) {
			perror("send");
			close(fd);
			return 1;
		}
	}

	close(fd);
	return 0;
}
