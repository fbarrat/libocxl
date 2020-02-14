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

/* global mmio registers */
#define LPC_AFU_GLOBAL_CFG	0

void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-v] [-n] [-D] [-d <device>]\n", prog);
	fprintf(stderr, "-v\t\tverbose mode\n");
	fprintf(stderr, "-n\t\tdon't do IBM,LPC AFU mmio setup\n");
	fprintf(stderr, "-D\t\toffline previously onlined LPC memory\n");
	fprintf(stderr, "-d <device>\tuse device instead of first LPC image found\n");
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
	uint64_t reg;

	// Map the full global MMIO area of the AFU
	if (ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global) != OCXL_OK) {
		return true;
	}

	if (ocxl_mmio_read64(global, LPC_AFU_GLOBAL_CFG, OCXL_MMIO_LITTLE_ENDIAN, &cfg) != OCXL_OK) {
		LOG_ERR("Reading global config register failed\n");
		return true;
	}
	LOG_INF("AFU config = 0x%lx\n", cfg);

	reg = 0x0000000000000015;
	if (ocxl_mmio_write64(global, LPC_AFU_GLOBAL_CFG, OCXL_MMIO_LITTLE_ENDIAN, reg) != OCXL_OK) {
		LOG_ERR("Writing Global Config Register failed\n");
		return true;
	}

	return 0;
}

int main(int argc, char **argv)
{
	bool verbose = false, do_mmio_setup = true;
	bool disable = false;
	int opt;
	char *device = NULL;
	ocxl_afu_h afu;
	ocxl_err err;

	while ((opt = getopt(argc, argv, "vnd:D")) != -1) {
		switch (opt) {
		case 'v':
			verbose = true;
			break;
		case 'n':
			do_mmio_setup = false;
			break;
		case 'd':
			device = optarg;
			break;
		case 'D':
			disable = true;
			break;
		default:
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (verbose) {
		ocxl_enable_messages(OCXL_ERRORS | OCXL_TRACING);
		ocxl_afu_enable_messages(afu, OCXL_ERRORS | OCXL_TRACING);
	} else {
		ocxl_enable_messages(OCXL_ERRORS);
		ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	}

	if (device)
		err = ocxl_afu_open_from_dev(device, &afu);
	else
		err = ocxl_afu_open(AFU_NAME, &afu);
	if (err != OCXL_OK) {
		LOG_ERR("ocxl_afu_open() failed for %s, error %d\n",
			device ? device : AFU_NAME, err);
		exit(1);
	}

	if (!disable) {
		if (do_mmio_setup && global_setup(afu)) {
			exit(1);
		}

		LOG_INF("lpc_mem_size=%lx\n", ocxl_afu_get_lpc_mem_size(afu));
		LOG_INF("lpc_mem_nodeid=%d\n", ocxl_afu_get_lpc_mem_nodeid(afu));

		err = ocxl_afu_online_lpc_mem(afu, 0);
		if (err != OCXL_OK) {
			LOG_ERR("Could not online AFU lpc memory: %d\n", err);
			exit(1);
		}
	} else {
		err = ocxl_afu_offline_lpc_mem(afu);
		if (err != OCXL_OK) {
			LOG_ERR("Could not offline AFU lpc memory: %d\n", err);
			exit(1);
		}
	}
	return 0;
}
