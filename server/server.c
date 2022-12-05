/*
 * Server echo application 
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct frame_t_ {
	uint32_t key;
	uint8_t data[];
} frame_t;

static char buffer[1024];

int main(int argc, char *argv[])
{
	frame_t *frame = (frame_t *)buffer;
	int fd, len, lendata, rc;
	
	if (argc !=  2 || (fd = open(argv[1], O_RDWR)) < 0) {
		printf("Error open()\n");
		return -1;
	}
	
	for(;;) {
		len = read(fd, frame, sizeof(buffer));
		if (len < sizeof(frame_t)) {
			printf("Error read len = %d\n", len);
			close(fd);
			return -1;
		}

		lendata = len - sizeof(frame_t);

		printf("RECV[%d]: key[%08x], data: %.*s\n", len, frame->key, lendata, frame->data);

		frame->key++;

		if ((rc = write(fd, frame, len)) != len) {
			printf("Error write len = %d\n", rc);
			close(fd);
			return -1;
		}

		printf("SEND[%d]: key[%08x], data: %.*s\n", len, frame->key, lendata, frame->data);
	}

	close(fd);
}



