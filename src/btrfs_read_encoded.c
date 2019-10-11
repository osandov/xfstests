#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
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

#include <lzo/lzo1x.h>
#include <zlib.h>
#include <zstd.h>

static const char *progname = "btrfs_encoded_read";
static int verbose = 0;

static int decompress_zlib(char *unencoded_buf, size_t unencoded_len,
			   const char *encoded_buf, size_t encoded_len)
{
	z_stream strm;
	int status = -1, ret;

	strm.next_in = (void *)encoded_buf;
	strm.avail_in = encoded_len;
	strm.next_out = (void *)unencoded_buf;
	strm.avail_out = unencoded_len;
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;

	ret = inflateInit(&strm);
	if (ret != Z_OK) {
		fprintf(stderr, "inflateInit: %d\n", ret);
		return -1;
	}

	while (strm.avail_in > 0 && strm.avail_out > 0) {
		if (verbose >= 2) {
			fprintf(stderr, "zlib in = %zu/%zu out = %zu/%zu\n",
				encoded_len - strm.avail_in, encoded_len,
				unencoded_len - strm.avail_out, unencoded_len);
		}
		ret = inflate(&strm, Z_FINISH);
		if (ret == Z_STREAM_END) {
			break;
		} else if (ret != Z_OK) {
			fprintf(stderr, "inflate: %d\n", ret);
			goto out;
		}
	}
	if (verbose >= 2) {
		fprintf(stderr, "zlib %sin = %zu/%zu out = %zu/%zu\n",
			ret == Z_STREAM_END ? "stream end " : "",
			encoded_len - strm.avail_in, encoded_len,
			unencoded_len - strm.avail_out, unencoded_len);
	}
	status = 0;
out:
	inflateEnd(&strm);
	return status;
}

static int decompress_zstd(char *unencoded_buf, size_t unencoded_len,
			   const char *encoded_buf, size_t encoded_len)
{
	ZSTD_DStream *zds;
	ZSTD_inBuffer in_buf = {
		.src = encoded_buf,
		.size = encoded_len,
	};
	ZSTD_outBuffer out_buf = {
		.dst = unencoded_buf,
		.size = unencoded_len,
	};
	size_t ret;
	int status = -1;

	zds = ZSTD_createDStream();
	if (!zds) {
		fprintf(stderr, "ZSTD_createDStream failed\n");
		return -1;
	}
	ret = ZSTD_initDStream(zds);
	if (ZSTD_isError(ret)) {
		fprintf(stderr, "ZSTD_initDStream: %s\n",
			ZSTD_getErrorName(ret));
		goto out;
	}

	while (in_buf.pos < in_buf.size && out_buf.pos < out_buf.size) {
		if (verbose >= 2) {
			fprintf(stderr, "zstd decompress in = %zu/%zu out = %zu/%zu\n",
				in_buf.pos, in_buf.size, out_buf.pos,
				out_buf.size);
		}
		ret = ZSTD_decompressStream(zds, &out_buf, &in_buf);
		if (ret == 0) {
			break;
		} else if (ZSTD_isError(ret)) {
			fprintf(stderr, "ZSTD_decompressStream: %s\n",
				ZSTD_getErrorName(ret));
			goto out;
		}
	}
	if (verbose >= 2) {
		fprintf(stderr, "zstd %sin = %zu/%zu out = %zu/%zu\n",
			ret == 0 ? "frame end " : "", in_buf.pos, in_buf.size,
			out_buf.pos, out_buf.size);
	}
	status = 0;
out:
	ZSTD_freeDStream(zds);
	return status;
}

static int decompress_lzo(char *unencoded_buf, size_t unencoded_len,
			  const char *encoded_buf, size_t encoded_len,
			  unsigned int page_size)
{
	uint32_t total_len;
	size_t in_pos, out_pos;

	if (encoded_len < 4) {
		fprintf(stderr, "lzo header is truncated\n");
		return -1;
	}
	memcpy(&total_len, encoded_buf, 4);
	total_len = le32toh(total_len);
	if (total_len > encoded_len) {
		fprintf(stderr, "lzo header is invalid\n");
		return -1;
	}

	in_pos = 4;
	out_pos = 0;
	while (in_pos < total_len && out_pos < unencoded_len) {
		size_t page_remaining;
		uint32_t src_len;
		lzo_uint dst_len;
		int ret;

		if (verbose >= 2) {
			fprintf(stderr, "lzo in = %zu/%" PRIu32 " out = %zu/%zu\n",
				in_pos, total_len, out_pos, unencoded_len);
		}

		page_remaining = -in_pos % page_size;
		if (page_remaining < 4) {
			if (total_len - in_pos <= page_remaining)
				break;
			in_pos += page_remaining;
		}

		if (total_len - in_pos < 4) {
			fprintf(stderr, "lzo segment header is truncated\n");
			return -1;
		}

		memcpy(&src_len, encoded_buf + in_pos, 4);
		src_len = le32toh(src_len);
		in_pos += 4;
		if (src_len > total_len - in_pos) {
			fprintf(stderr, "lzo segment header is invalid\n");
			return -1;
		}

		dst_len = page_size;
		ret = lzo1x_decompress_safe((void *)(encoded_buf + in_pos),
					    src_len,
					    (void *)(unencoded_buf + out_pos),
					    &dst_len, NULL);
		if (ret != LZO_E_OK) {
			fprintf(stderr, "lzo1x_decompress_safe: %d\n", ret);
			return -1;
		}

		in_pos += src_len;
		out_pos += dst_len;
	}
	if (verbose >= 2) {
		fprintf(stderr, "lzo in = %zu/%" PRIu32 " out = %zu/%zu\n",
			in_pos, total_len, out_pos, unencoded_len);
	}
	return 0;
}

static ssize_t print_extent(const struct encoded_iov *encoded,
			    const char *encoded_buf, size_t encoded_len,
			    size_t count)
{
	char *unencoded_buf;
	ssize_t ret;
	int page_shift;

	if (encoded->len < count)
		count = encoded->len;

	if (encoded->encryption) {
		fprintf(stderr, "unknown encryption %u\n", encoded->encryption);
		return -1;
	}

	if (encoded->unencoded_len == 0)
		return 0;

	unencoded_buf = calloc(encoded->unencoded_len, 1);
	if (!unencoded_buf) {
		perror("calloc");
		return -1;
	}

	switch (encoded->compression) {
	case ENCODED_IOV_COMPRESSION_NONE:
		memcpy(unencoded_buf, encoded_buf,
		       encoded_len < encoded->unencoded_len ?
		       encoded_len : encoded->unencoded_len);
		break;
	case ENCODED_IOV_COMPRESSION_BTRFS_ZLIB:
		ret = decompress_zlib(unencoded_buf, encoded->unencoded_len,
				      encoded_buf, encoded_len);
		if (ret)
			goto out;
		break;
	case ENCODED_IOV_COMPRESSION_BTRFS_ZSTD:
		ret = decompress_zstd(unencoded_buf, encoded->unencoded_len,
				      encoded_buf, encoded_len);
		if (ret)
			goto out;
		break;
	case ENCODED_IOV_COMPRESSION_BTRFS_LZO_4K:
	case ENCODED_IOV_COMPRESSION_BTRFS_LZO_8K:
	case ENCODED_IOV_COMPRESSION_BTRFS_LZO_16K:
	case ENCODED_IOV_COMPRESSION_BTRFS_LZO_32K:
	case ENCODED_IOV_COMPRESSION_BTRFS_LZO_64K:
		page_shift = (encoded->compression -
			      ENCODED_IOV_COMPRESSION_BTRFS_LZO_4K + 12);
		ret = decompress_lzo(unencoded_buf, encoded->unencoded_len,
				     encoded_buf, encoded_len,
				     1U << page_shift);
		if (ret)
			goto out;
		break;
	default:
		fprintf(stderr, "unknown compression %u\n", encoded->compression);
		ret = -1;
		goto out;
	}

	if (encoded->unencoded_offset < encoded->unencoded_len) {
		ret = encoded->unencoded_len - encoded->unencoded_offset;
		if (count < ret)
			ret = count;
		fwrite(unencoded_buf + encoded->unencoded_offset, 1, ret,
		       stdout);
	} else {
		ret = 0;
	}
out:
	free(unencoded_buf);
	return ret;
}

static void usage(bool error)
{
	fprintf(error ? stderr : stdout,
		"usage: %s [OPTION]... [PATH]...\n"
		"\n"
		"Read a file from Btrfs using RWF_ENCODED."
		"\n"
		"Options:\n"
		"  -o, --offset=BYTES         start reading at the given offset\n"
		"  -l, --length=BYTES         read the given number of bytes\n"
		"  -b, --buffer-size=BYTES    use the given size for the read buffer\n"
		"  -v, --verbose              print debugging information to stderr\n"
		"  -h, --help                 display this help message and exit\n",
		progname);
	exit(error ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	struct option long_options[] = {
		{"offset", required_argument, NULL, 'o'},
		{"length", required_argument, NULL, 'l'},
		{"buffer-size", required_argument, NULL, 'b'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
	};
	long long offset = 0;
	long long length = LONG_LONG_MAX;
	int fd;
	struct encoded_iov encoded;
	struct iovec iov[2] = {
		{ &encoded, sizeof(encoded), },
		{ .iov_len = 128 * 1024, },
	};
	int status = EXIT_FAILURE;

	for (;;) {
		int c;

		c = getopt_long(argc, argv, "o:l:b:vh", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'o':
			offset = atoll(optarg);
			break;
		case 'l':
			length = atoll(optarg);
			break;
		case 'b':
			iov[1].iov_len = strtoul(optarg, NULL, 0);
			if (!iov[1].iov_len)
				usage(true);
			break;
		case 'v':
			verbose++;
			break;
		case 'h':
			usage(false);
		default:
			usage(true);
		}
	}
	if (argc - optind != 1)
		usage(true);

	iov[1].iov_base = malloc(iov[1].iov_len);
	if (!iov[1].iov_base) {
		perror("malloc");
		return EXIT_FAILURE;
	}

	fd = open(argv[optind], O_RDONLY | O_ALLOW_ENCODED);
	if (fd == -1) {
		perror("open");
		goto out_free_buf;
	}

	if (offset && lseek(fd, offset, SEEK_SET) == -1) {
		perror("lseek");
		goto out;
	}

	while (length) {
		ssize_t ret;

		if (verbose >= 1) {
			offset = lseek(fd, 0, SEEK_CUR);
			if (offset == -1) {
				perror("lseek");
				goto out;
			}
		}
		ret = preadv2(fd, iov, 2, -1, RWF_ENCODED);
		if (ret == -1) {
			if (errno == EOPNOTSUPP)
				status = 2;
			perror("preadv2");
			goto out;
		}

		if (verbose >= 1) {
			fflush(stdout);
			fprintf(stderr,
				"\noffset = %lld, len = %llu, encoded_len = %zd, unencoded_len = %llu, unencoded_offset = %llu, compression = ",
				offset, (unsigned long long)encoded.len, ret,
				(unsigned long long)encoded.unencoded_len,
				(unsigned long long)encoded.unencoded_offset);
			switch (encoded.compression) {
			case ENCODED_IOV_COMPRESSION_NONE:
				fprintf(stderr, "none\n");
				break;
			case ENCODED_IOV_COMPRESSION_BTRFS_ZLIB:
				fprintf(stderr, "zlib\n");
				break;
			case ENCODED_IOV_COMPRESSION_BTRFS_ZSTD:
				fprintf(stderr, "zstd\n");
				break;
			case ENCODED_IOV_COMPRESSION_BTRFS_LZO_4K:
			case ENCODED_IOV_COMPRESSION_BTRFS_LZO_8K:
			case ENCODED_IOV_COMPRESSION_BTRFS_LZO_16K:
			case ENCODED_IOV_COMPRESSION_BTRFS_LZO_32K:
			case ENCODED_IOV_COMPRESSION_BTRFS_LZO_64K:
				fprintf(stderr, "lzo\n");
				break;
			default:
				fprintf(stderr, "%u\n", encoded.compression);
				break;
			}
			fflush(stderr);
		}
		if (ret == 0)
			break;
		ret = print_extent(&encoded, iov[1].iov_base, ret, length);
		if (ret == -1)
			goto out;
		length -= ret;
	}

	status = EXIT_SUCCESS;
out:
	close(fd);
out_free_buf:
	free(iov[1].iov_base);
	return status;
}
