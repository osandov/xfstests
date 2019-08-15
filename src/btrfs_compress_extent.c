#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <lzo/lzo1x.h>
#include <zlib.h>
#include <zstd.h>

static const char *progname = "btrfs_compress_extent";

#define lzo1x_worst_compress(x) ((x) + ((x) / 16) + 64 + 3 + 2)
#define ZSTD_BTRFS_MAX_WINDOWLOG 17

static int write_all(int fd, const void *buf, size_t count)
{
	const char *p = buf;

	while (count) {
		ssize_t sret;

		sret = write(fd, p, count);
		if (sret == -1) {
			if (errno == EINTR)
				continue;
			perror("write");
			return -1;
		}
		p += sret;
		count -= sret;
	}
	return 0;
}

static int do_compress_zlib(int in_fd, int out_fd)
{
	z_stream strm;
	Bytef in_buf[4096];
	Bytef out_buf[4096];
	int flush = Z_NO_FLUSH;
	int status = -1, ret;

	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;

	ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
	if (ret != Z_OK) {
		fprintf(stderr, "deflateInit: %d\n", ret);
		return -1;
	}

	strm.next_in = in_buf;
	strm.avail_in = 0;
	strm.next_out = out_buf;
	strm.avail_out = sizeof(out_buf);
	do {
		if (flush != Z_FINISH && !strm.avail_in) {
			ssize_t sret;

			sret = read(in_fd, in_buf, sizeof(in_buf));
			if (sret == -1) {
				if (errno == EINTR)
					continue;
				perror("read");
				goto out;
			}
			if (sret == 0) {
				flush = Z_FINISH;
			} else {
				strm.next_in = in_buf;
				strm.avail_in = sret;
			}
		}

		ret = deflate(&strm, flush);
		if (ret != Z_OK && ret != Z_STREAM_END) {
			fprintf(stderr, "deflate: %d\n", ret);
			goto out;
		}

		if (write_all(out_fd, out_buf, strm.next_out - out_buf) == -1)
			goto out;
		strm.next_out = out_buf;
		strm.avail_out = sizeof(out_buf);
	} while (ret != Z_STREAM_END);
	status = 0;
out:
	deflateEnd(&strm);
	return status;
}

static int do_compress_lzo(int in_fd, int out_fd)
{
	long page_size;
	size_t worst_compress;
	void *mem, *src, *dst = NULL;
	size_t dst_size, dst_capacity;
	int status = -1;

	page_size = sysconf(_SC_PAGE_SIZE);
	if (page_size == -1) {
		perror("sysconf");
		return -1;
	}
	worst_compress = lzo1x_worst_compress(page_size);
	mem = malloc(LZO1X_MEM_COMPRESS);
	if (!mem) {
		perror("malloc");
		return -1;
	}

	src = malloc(page_size);
	if (!src) {
		perror("malloc");
		goto out_mem;
	}

	dst_size = dst_capacity = 4;
	dst = malloc(dst_capacity);
	if (!dst) {
		perror("malloc");
		goto out_src;
	}

	for (;;) {
		long src_len = 0;
		lzo_uint dst_len;
		int ret;

		while (src_len < page_size) {
			ssize_t sret;

			sret = read(in_fd, src, page_size -  src_len);
			if (sret == -1) {
				if (errno == EINTR)
					continue;
				perror("read");
				goto out_dst;
			}
			if (sret == 0)
				break;
			src_len += sret;
		}
		if (src_len == 0)
			break;

		if (dst_capacity - dst_size < 4 + worst_compress) {
			void *tmp;

			do {
				dst_capacity *= 2;
			} while (dst_capacity - dst_size < 4 + worst_compress);
			tmp = realloc(dst, dst_capacity);
			if (!tmp) {
				perror("realloc");
				goto out_dst;
			}
			dst = tmp;
		}

		ret = lzo1x_1_compress(src, src_len,
				       (void *)((char *)dst + dst_size + 4),
				       &dst_len, mem);
		if (ret != LZO_E_OK) {
			fprintf(stderr, "lzo1x_1_compress: %d\n", ret);
			goto out_dst;
		}
		*(uint32_t *)((char *)dst + dst_size) = htole32(dst_len);
		dst_size += 4 + dst_len;
	}

	*(uint32_t *)dst = htole32(dst_size);
	status = write_all(out_fd, dst, dst_size);
out_dst:
	free(dst);
out_src:
	free(src);
out_mem:
	free(mem);
	return status;
}

static int do_compress_zstd(int in_fd, int out_fd)
{
	ZSTD_CStream *zcs;
	size_t ret, in_size;
	ZSTD_inBuffer in_buf = {};
	ZSTD_outBuffer out_buf = {};
	int status = -1;

	zcs = ZSTD_createCStream();
	if (!zcs) {
		fprintf(stderr, "ZSTD_createCStream failed\n");
		return -1;
	}

	ret = ZSTD_initCStream(zcs, ZSTD_CLEVEL_DEFAULT);
	if (ZSTD_isError(ret)) {
		fprintf(stderr, "ZSTD_initCStream: %s\n",
			ZSTD_getErrorName(ret));
		goto out_zcs;
	}
	ret = ZSTD_CCtx_setParameter(zcs, ZSTD_c_windowLog,
				     ZSTD_BTRFS_MAX_WINDOWLOG);
	if (ZSTD_isError(ret)) {
		fprintf(stderr, "ZSTD_CCtx_setParameter: %s\n",
			ZSTD_getErrorName(ret));
		goto out_zcs;
	}

	in_size = ZSTD_CStreamInSize();
	in_buf.src = malloc(in_size);
	in_buf.size = in_buf.pos = in_size;
	if (!in_buf.src) {
		perror("malloc");
		goto out_zcs;
	}
	out_buf.size = ZSTD_CStreamOutSize();
	out_buf.dst = malloc(out_buf.size);
	if (!out_buf.dst) {
		perror("malloc");
		goto out_in_buf;
	}

	for (;;) {
		if (in_buf.pos == in_buf.size) {
			ssize_t sret;

			sret = read(in_fd, (void *)in_buf.src, in_size);
			if (sret == -1) {
				if (errno == EINTR)
					continue;
				perror("read");
				goto out_out_buf;
			}
			if (sret == 0)
				break;
			in_buf.pos = 0;
			in_buf.size = sret;
		}

		ret = ZSTD_compressStream(zcs, &out_buf, &in_buf);
		if (ZSTD_isError(ret)) {
			fprintf(stderr, "ZSTD_compressStream: %s\n",
				ZSTD_getErrorName(ret));
			goto out_out_buf;
		}

		if (write_all(out_fd, out_buf.dst, out_buf.pos) == -1)
			goto out_out_buf;
		out_buf.pos = 0;
	}
	do {
		ret = ZSTD_endStream(zcs, &out_buf);
		if (ZSTD_isError(ret)) {
			fprintf(stderr, "ZSTD_endStream: %s\n",
				ZSTD_getErrorName(ret));
			goto out_out_buf;
		}

		if (write_all(out_fd, out_buf.dst, out_buf.pos) == -1)
			goto out_out_buf;
		out_buf.pos = 0;
	} while (ret);

	status = 0;
out_out_buf:
	free(out_buf.dst);
out_in_buf:
	free((void *)in_buf.src);
out_zcs:
	ZSTD_freeCStream(zcs);
	return status;
}

static void usage(bool error)
{
	fprintf(error ? stderr : stdout,
		"usage: %s [OPTION]...\n"
		"\n"
		"Read data from stdin, compress it in the format of a Btrfs extent, and\n"
		"write it to stdout.\n"
		"\n"
		"Options:\n"
		"  -t, --type=TYPE         set the compression type ('zlib', 'lzo', or\n"
		"                          'zstd'); the default is 'zlib'\n"
		"  -h, --help              display this help message and exit\n",
		progname);
	exit(error ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	struct option long_options[] = {
		{"type", required_argument, NULL, 't'},
		{"help", no_argument, NULL, 'h'},
	};
	enum {
		ZLIB,
		LZO,
		ZSTD,
	} compression = ZLIB;
	int ret;

	progname = argv[0];

	for (;;) {
		int c;

		c = getopt_long(argc, argv, "t:h", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 't':
			if (strcmp(optarg, "zlib") == 0)
				compression = ZLIB;
			else if (strcmp(optarg, "lzo") == 0)
				compression = LZO;
			else if (strcmp(optarg, "zstd") == 0)
				compression = ZSTD;
			else
				usage(true);
			break;
		case 'h':
			usage(false);
		default:
			usage(true);
		}
	}
	if (argc - optind)
		usage(true);

	switch (compression) {
	case ZLIB:
		ret = do_compress_zlib(STDIN_FILENO, STDOUT_FILENO);
		break;
	case LZO:
		ret = do_compress_lzo(STDIN_FILENO, STDOUT_FILENO);
		break;
	case ZSTD:
		ret = do_compress_zstd(STDIN_FILENO, STDOUT_FILENO);
		break;
	}
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
