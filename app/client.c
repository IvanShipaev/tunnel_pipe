/*
 * Client application 
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
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
	
	if (argc !=  3 || (fd = open(argv[1], O_RDWR)) < 0) {
		printf("Error open file: %s\n", argv[1]);
		return -1;
	}

	srand(time(NULL) ^ getpid());

	frame->key = rand();
	
	lendata = snprintf(frame->data, sizeof(buffer) - sizeof(frame_t), "%s", argv[2]);

	len = sizeof(frame_t) + lendata;

	if ((rc=write(fd, frame, len)) != len) {
		printf("Error write %d != %d\n", rc, len);
		close(fd);
		return -1;
	}

	printf("SEND[%d]: key[%08x], data: %.*s\n", len, frame->key, lendata, frame->data);

	len = read(fd, frame, sizeof(buffer));
	if (len < sizeof(frame_t)) { 
		printf("Error len frame %d\n", len);
		close(fd);
		return -1;
	}
		
	lendata = len - sizeof(frame_t);
	printf("RECV[%d]: key[%08x], data: %.*s\n", len, frame->key, lendata, frame->data);

	close(fd);
}
