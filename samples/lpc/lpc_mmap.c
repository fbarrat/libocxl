#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include "libocxl.h"

#define LOG_ERR(fmt, x...) fprintf(stderr, fmt, ##x)
#define LOG_INF(fmt, x...) printf(fmt, ##x)

#define AFU_NAME "IBM,LPC"

#define GLOBAL_MMIO_REG_COUNT (0x300/8)

/* global mmio registers */
#define LPC_AFU_GLOBAL_CFG	0

void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-m] [-o offset] [-s size] [-w] [-r] [-n]\n", prog);
	fprintf(stderr, "-m\tDump global MMIO\n");
	fprintf(stderr, "-o\tOffset to start testing at (must be 64 bit aligned)\n");
	fprintf(stderr, "-r\tValidate LPC contents\n");
	fprintf(stderr, "-s\tSize to test in bytes (must be a multiple of 64 bits)\n");
	fprintf(stderr, "-w\tWrite LPC contents\n");
	fprintf(stderr, "-n\tDon't do IBM,LPC AFU mmio setup\n");
}

void dump_global_mmio(ocxl_afu_h afu)
{
	ocxl_mmio_h global;
	ocxl_err rc;

	rc = ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global);
	if (rc != OCXL_OK) {
		fprintf(stderr, "Could not map global MMIO\n");
		exit(1);
	}

	for (size_t reg = 0; reg < GLOBAL_MMIO_REG_COUNT; reg++) {
		uint64_t val;
		uint64_t addr = reg * sizeof(uint64_t);
		rc = ocxl_mmio_read64(global, addr, OCXL_MMIO_LITTLE_ENDIAN, &val);
		if (rc != OCXL_OK) {
			fprintf(stderr, "Failed to read MMIO register at %#0lx", addr);
			exit(1);
		}

		printf("%#0lx=%#0lx\n", addr, val);
	}
}

/**
 * Set up the Global MMIO area of the AFU
 *
 * @param afu the AFU handle
 * @return false on success
 */
static bool global_setup(ocxl_afu_h afu)
{
	uint64_t cfg;
	ocxl_mmio_h global;

	// Map the full global MMIO area of the AFU
	if (OCXL_OK != ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global)) {
		return true;
	}

	if (OCXL_OK != ocxl_mmio_read64(global, LPC_AFU_GLOBAL_CFG, OCXL_MMIO_LITTLE_ENDIAN, &cfg)) {
		LOG_ERR("Reading global config register failed\n");
		return true;
	}
	LOG_INF("AFU config = 0x%lx\n", cfg);

	uint64_t reg = 0x0000000000000015;
	if (OCXL_OK != ocxl_mmio_write64(global, LPC_AFU_GLOBAL_CFG, OCXL_MMIO_LITTLE_ENDIAN, reg)) {
		LOG_ERR("Writing Global Config Register failed\n");
		return true;
	}

	return 0;
}


void write_lpc(ocxl_afu_h afu, size_t offset, size_t size)
{
	ocxl_mmio_h lpc;

	fprintf(stderr, "Mapping LPC memory\n");
	//sleep(5);
	ocxl_err rc = ocxl_mmio_map_advanced(afu, OCXL_LPC_SYSTEM_MEM, size, PROT_READ | PROT_WRITE,
	                                     0, offset, &lpc);
	if (rc != OCXL_OK) {
		fprintf(stderr, "Could not map LPC memory\n");
		exit(1);
	}
	fprintf(stderr, "LPC mapped\n");
	//sleep(5);

	void *lpc_addr;
	size_t lpc_size;

	rc = ocxl_mmio_get_info(lpc, &lpc_addr, &lpc_size);
	if (rc != OCXL_OK) {
		fprintf(stderr, "Could not fetch LPC info\n");
		exit(1);
	}
	fprintf(stderr, "Got EA for LPC memory: %p\n", lpc_addr);
	//sleep(5);

	uint64_t *vals = lpc_addr;
	size_t val_count = size / sizeof(uint64_t);

	fprintf(stderr, "Populating %ld 64 bit values (%ld bytes) starting at offset %#0lx\n",
	        val_count, val_count * sizeof(uint64_t), offset);
	//sleep(5);
	for (size_t i = 0; i < val_count; i++) {
		vals[i] = i * sizeof(uint64_t) + offset;
	}
}


void read_lpc(ocxl_afu_h afu, size_t offset, size_t size)
{
	ocxl_mmio_h lpc;

	ocxl_err rc = ocxl_mmio_map_advanced(afu, OCXL_LPC_SYSTEM_MEM, size, PROT_READ | PROT_WRITE,
	                                     0, offset, &lpc);
	if (rc != OCXL_OK) {
		fprintf(stderr, "Could not map LPC memory\n");
		exit(1);
	}

	void *lpc_addr;
	size_t lpc_size;

	rc = ocxl_mmio_get_info(lpc, &lpc_addr, &lpc_size);
	if (rc != OCXL_OK) {
		fprintf(stderr, "Could not fetch LPC info\n");
		exit(1);
	}
	printf("Got EA for LPC memory: %p\n", lpc_addr);

	uint64_t *vals = lpc_addr;
	size_t val_count = size / sizeof(uint64_t);

	printf("Validating %ld 64 bit values (%ld bytes) starting at offset %#0lx\n",
	       val_count, val_count * sizeof(uint64_t), offset);
	fflush(stdout);
	//sleep(5);
	for (size_t i = 0; i < val_count; i++) {
		uint64_t expected = offset + i * sizeof(uint64_t);
		if (vals[i] != expected) {
			fprintf(stderr, "Validation failed, value %ld at offset %#0lx expected %#0lx, got %#0lx\n",
			        i, offset, expected, vals[i]);
		}
	}

	printf("Validation complete\n");
}

int main(int argc, char **argv)
{
	bool dump_mmio = false;
	uint64_t offset = 0;
	uint64_t size = 1ULL*1024*1024*124;
	bool verbose = false;
	bool read = false;
	bool write = false;
	bool do_mmio_setup = true;
	int opt;

	while ((opt = getopt(argc, argv, "mo:rs:vwn")) != -1) {
		switch (opt) {
		case 'm':
			dump_mmio = true;
			break;

		case 'o':
			offset = atoll(optarg);
			break;

		case 'r':
			read = true;
			break;

		case 's':
			size = atoll(optarg);
			break;

		case 'v':
			verbose = true;
			break;

		case 'w':
			write = true;
			break;

		case 'n':
			do_mmio_setup = false;
			break;

		default: /* '?' */
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (verbose) {
		ocxl_enable_messages(OCXL_ERRORS | OCXL_TRACING);
	} else {
		ocxl_enable_messages(OCXL_ERRORS);
	}

	ocxl_afu_h afu;
	if (OCXL_OK != ocxl_afu_open(AFU_NAME, &afu)) {
		fprintf(stderr, "Could not open AFU '%s'\n", AFU_NAME);
		exit(1);
	}
	printf("lpc_mem_size=%lx\n", ocxl_afu_get_lpc_mem_size(afu));

	// Enable per-AFU messages
	if (verbose) {
		ocxl_afu_enable_messages(afu, OCXL_ERRORS | OCXL_TRACING);
	} else {
		ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	}

	if (do_mmio_setup && global_setup(afu)) {
		exit(1);
	}

	if (dump_mmio) {
		dump_global_mmio(afu);
	}

	if (write) {
		write_lpc(afu, offset, size);
	}

	if (read) {
		read_lpc(afu, offset, size);
	}

	return 0;
}
