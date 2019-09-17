#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <linux/fs.h>
#include "encoded_io.h"

static const char *progname = "encoded_write";

static void *read_all(int fd, size_t *count)
{
	char *buf;
	size_t capacity = 4096;

	*count = 0;
	buf = malloc(capacity);
	if (!buf) {
		perror("malloc");
		return NULL;
	}
	for (;;) {
		ssize_t sret;

		if (*count == capacity) {
			void *tmp;

			capacity *= 2;
			tmp = realloc(buf, capacity);
			if (!tmp) {
				perror("realloc");
				free(buf);
				return NULL;
			}
			buf = tmp;
		}

		sret = read(fd, buf + *count, capacity - *count);
		if (sret == -1) {
			if (errno == EINTR)
				continue;
			perror("read");
			free(buf);
			return NULL;
		}
		if (sret == 0)
			break;
		*count += sret;
	}
	return buf;
}

static void usage(bool error)
{
	fprintf(error ? stderr : stdout,
		"usage: %1$s [OPTION]... PATH OFFSET LEN\n"
		"\n"
		"Read an encoded extent from stdin and write it with pwritev2() using\n"
		"RWF_ENCODED.\n"
		"\n"
		"Exits with status 0 on success, 2 on EOPNOTSUPP, and 1 on any other\n"
		"error.\n"
		"\n"
		"Options:\n"
		"  -c, --compression=TYPE    set the compression type ('none', 'zlib',\n"
		"                            'lzo', or 'zstd'); the default is 'none'\n"
		"  -l, --length=BYTES        set the unencoded length if it is different\n"
		"                            from the length in the file\n"
		"  -o, --offset=BYTES        set the unencoded offset if it is non-zero\n"
		"  -h, --help                display this help message and exit\n",
		progname);
	exit(error ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	struct option long_options[] = {
		{"compression", required_argument, NULL, 'c'},
		{"length", required_argument, NULL, 'l'},
		{"offset", required_argument, NULL, 'o'},
		{"help", no_argument, NULL, 'h'},
	};
	struct encoded_iov encoded = {};
	struct iovec iov[2] = {
		{ &encoded, sizeof(encoded) },
	};
	bool set_unencoded_len = false;
	const char *path;
	unsigned long long offset;
	int fd;
	int status = EXIT_FAILURE;

	progname = argv[0];

	for (;;) {
		int c;

		c = getopt_long(argc, argv, "c:l:o:h", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			if (strcmp(optarg, "none") == 0) {
				encoded.compression =
					ENCODED_IOV_COMPRESSION_NONE;
			} else if (strcmp(optarg, "zlib") == 0) {
				encoded.compression =
					ENCODED_IOV_COMPRESSION_BTRFS_ZLIB;
			} else if (strcmp(optarg, "lzo") == 0) {
				switch (sysconf(_SC_PAGE_SIZE)) {
				case 4096:
					encoded.compression =
						ENCODED_IOV_COMPRESSION_BTRFS_LZO_4K;
					break;
				case 8192:
					encoded.compression =
						ENCODED_IOV_COMPRESSION_BTRFS_LZO_8K;
					break;
				case 16384:
					encoded.compression =
						ENCODED_IOV_COMPRESSION_BTRFS_LZO_16K;
					break;
				case 32768:
					encoded.compression =
						ENCODED_IOV_COMPRESSION_BTRFS_LZO_32K;
					break;
				case 65536:
					encoded.compression =
						ENCODED_IOV_COMPRESSION_BTRFS_LZO_64K;
					break;
				default:
					fprintf(stderr, "unsupported page size");
					return EXIT_FAILURE;
				}
			} else if (strcmp(optarg, "zstd") == 0) {
				encoded.compression =
					ENCODED_IOV_COMPRESSION_BTRFS_ZSTD;
			} else {
				usage(true);
			}
			break;
		case 'l':
			encoded.unencoded_len = strtoull(optarg, NULL, 0);
			set_unencoded_len = true;
			break;
		case 'o':
			encoded.unencoded_offset = strtoull(optarg, NULL, 0);
			break;
		case 'h':
			usage(false);
		default:
			usage(true);
		}
	}
	if (argc - optind != 3)
		usage(true);

	path = argv[optind];
	offset = strtoull(argv[optind + 1], NULL, 0);
	encoded.len = strtoull(argv[optind + 2], NULL, 0);
	if (!set_unencoded_len)
		encoded.unencoded_len = encoded.len;

	fd = open(path, O_WRONLY | O_CREAT | O_ALLOW_ENCODED | O_CLOEXEC, 0666);
	if (fd == -1) {
		perror("open");
		return EXIT_FAILURE;
	}

	iov[1].iov_base = read_all(STDIN_FILENO, &iov[1].iov_len);
	if (!iov[1].iov_base)
		goto out;

	if (pwritev2(fd, iov, 2, offset, RWF_ENCODED) == -1) {
		if (errno == EOPNOTSUPP)
			status = 2;
		perror("pwritev2");
		goto out;
	}

	status = EXIT_SUCCESS;
out:
	close(fd);
	return status;
}
