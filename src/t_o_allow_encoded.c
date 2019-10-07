#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "encoded_io.h"

int main(int argc, char **argv)
{
	int fd;
	int flags;

	if (argc != 3) {
		fprintf(stderr, "usage: %s PATH UID\n", argv[0]);
		return 1;
	}

	unlink(argv[1]);
	umask(0);

	fd = open(argv[1], O_RDWR | O_CREAT, 0666);
	if (fd == -1) {
		perror("open");
		return 1;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		perror("fcntl(F_GETFL)");
		close(fd);
		return 1;
	}

	fprintf(stderr, "Set O_ALLOW_ENCODED with fcntl\n");
	if (fcntl(fd, F_SETFL, flags | O_ALLOW_ENCODED) == -1) {
		perror("fcntl(F_SETFL)");
		close(fd);
		return 1;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		perror("fcntl(F_GETFL)");
		close(fd);
		return 1;
	}
	if (!(flags & O_ALLOW_ENCODED)) {
		fprintf(stderr, "O_ALLOW_ENCODED was not set\n");
		close(fd);
		return 2;
	}

	fprintf(stderr, "Clear O_ALLOW_ENCODED with fcntl\n");
	if (fcntl(fd, F_SETFL, flags & ~O_ALLOW_ENCODED) == -1) {
		perror("fcntl(F_SETFL)");
		close(fd);
		return 1;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		perror("fcntl(F_GETFL)");
		close(fd);
		return 1;
	}
	if (flags & O_ALLOW_ENCODED) {
		fprintf(stderr, "O_ALLOW_ENCODED was not cleared\n");
		close(fd);
		return 1;
	}

	close(fd);

	fprintf(stderr, "Open with O_ALLOW_ENCODED\n");
	fd = open(argv[1], O_RDWR | O_ALLOW_ENCODED);
	if (fd == -1) {
		perror("open");
		return 1;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		perror("fcntl(F_GETFL)");
		close(fd);
		return 1;
	}
	if (!(flags & O_ALLOW_ENCODED)) {
		fprintf(stderr, "O_ALLOW_ENCODED was not set\n");
		close(fd);
		return 1;
	}

	seteuid(atoi(argv[2]));

	fprintf(stderr, "Change flags with O_ALLOW_ENCODED set (unprivileged)\n");
	if (fcntl(fd, F_SETFL, flags | O_CLOEXEC) == -1) {
		perror("fcntl(F_SETFL)");
		close(fd);
		return 1;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		perror("fcntl(F_GETFL)");
		close(fd);
		return 1;
	}
	if (!(flags & O_ALLOW_ENCODED)) {
		fprintf(stderr, "O_ALLOW_ENCODED was cleared\n");
		close(fd);
		return 1;
	}

	fprintf(stderr, "Clear O_ALLOW_ENCODED with fcntl (unprivileged)\n");
	if (fcntl(fd, F_SETFL, flags & ~O_ALLOW_ENCODED) == -1) {
		perror("fcntl(F_SETFL)");
		close(fd);
		return 1;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		perror("fcntl(F_GETFL)");
		close(fd);
		return 1;
	}
	if (flags & O_ALLOW_ENCODED) {
		fprintf(stderr, "O_ALLOW_ENCODED was not cleared\n");
		close(fd);
		return 1;
	}

	fprintf(stderr, "Set O_ALLOW_ENCODED with fcntl (unprivileged)\n");
	if (fcntl(fd, F_SETFL, flags | O_ALLOW_ENCODED) == 0) {
		fprintf(stderr, "unprivileged O_ALLOW_ENCODED succeeded\n");
		close(fd);
		return 1;
	}
	if (errno != EPERM) {
		perror("fcntl(F_SETFL)");
		close(fd);
		return 1;
	}

	close(fd);

	fprintf(stderr, "Open with O_ALLOW_ENCODED (unprivileged)\n");
	fd = open(argv[1], O_RDWR | O_ALLOW_ENCODED);
	if (fd != -1) {
		fprintf(stderr, "unprivileged O_ALLOW_ENCODED succeeded\n");
		close(fd);
		unlink(argv[1]);
		return 1;
	}
	if (errno != EPERM) {
		perror("open");
		return 1;
	}

	return 0;
}
