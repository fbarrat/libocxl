/*
 * Copyright 2017 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include "libocxl_internal.h"
#include "static.h"

#include <stdlib.h>

static const char *ocxl_sysfs_path = "/tmp/ocxl-test";
static const char *ocxl_dev_path = "/dev/ocxl-test";
static const char *ocxl_irq_path = "/dev/usrirq-test";

#define MAX_TESTS 1024
#define TEST_NAME_LEN	32
#define SUITE_NAME_LEN	32

#define GLOBAL_MMIO_SIZE	(32*1024*1024)
#define PER_PASID_MMIO_SIZE	16384

typedef enum state {
	IN_PROGRESS,
	SUCCESS,
	SKIP,
	FAIL
} state;

typedef struct test {
	char suite[SUITE_NAME_LEN+1];
	char name[TEST_NAME_LEN+1];
	state state;
	int count; /**< The number of checks performed */
} test;

test tests[MAX_TESTS];
int current_test = -1;

#define ASSERT(statement) do { \
	tests[current_test].count++; \
	if (!(statement)) { \
		fprintf(stderr, "ASSERT: %s:%d: %s(): '%s' is false\n", __FILE__, __LINE__, __FUNCTION__, #statement); \
		test_stop(FAIL); \
		goto end; \
	} \
} while(0)

#define SKIP(statement, msg) do { \
	if ((statement)) { \
		fprintf(stderr, "Skip: '%s' is true\n", #statement); \
		test_stop(SKIP); \
		goto end; \
	} \
} while(0)


// virtocxl functions
pthread_t create_ocxl_device(const char *afu_name, size_t global_mmio_size, size_t per_pasid_mmio_size);
pthread_t create_usrirq_device(const char *dev_name);
void stop_afu();
void stop_usrirq();
void term_afu();
void term_usrirq();
void force_translation_fault(void *addr, uint64_t dsisr);
bool afu_is_attached();

/**
 * Start a test
 * @param suite the name of the test suite
 * @param name the name of the test
 */
static void test_start(const char *suite, const char *name) {
	printf("Starting test %s:%s\n",
			suite, name);
	current_test++;
	if (current_test == MAX_TESTS) {
		fprintf(stderr, "Too many tests");
		exit(1);
	}

	if (current_test && tests[current_test-1].state == IN_PROGRESS) {
		fprintf(stderr, "Could not start '%s:%s', previous test '%s:%s' is still in progress",
				suite, name, tests[current_test-1].suite, tests[current_test-1].name);
	}

	strncpy(tests[current_test].suite, suite, sizeof(tests->suite));
	strncpy(tests[current_test].name, name, sizeof(tests->name));
	tests[current_test].state = IN_PROGRESS;
	tests[current_test].count = 0;
}

/**
 * Complete the current test
 * @param state the state of the test
 */
static void test_stop(state state) {
	tests[current_test].state = state;
	if (state == SUCCESS) {
		printf("OK\n");
	} else if (state == SKIP) {
		printf("Skipped\n");
	} else if (state == FAIL) {
		printf("Failed\n");
	}
}

/**
 * Report the state of the tests
 * @return true if we have failed tests
 */
static bool test_report() {
	bool has_failed = false;

	printf("Summary:\n");

	int max_suite_name_len = 0;
	int max_test_name_len = 0;

	for (int test = 0; test <= current_test; test++) {
		int len = strlen(tests[test].suite);
		if (max_suite_name_len < len) {
			max_suite_name_len = len;
		}

		len = strlen(tests[test].name);
		if (max_test_name_len < len) {
			max_test_name_len = len;
		}
	}

	for (int test = 0; test <= current_test; test++) {
		char *state;
		switch (tests[test].state) {
		case IN_PROGRESS:
			state = "In Progress";
			has_failed = true;
			break;
		case SUCCESS:
			state = "OK";
			break;
		case SKIP:
			state = "Skipped";
			break;
		case FAIL:
			state = "Failed";
			has_failed = true;
			break;
		default:
			state = "Unknown";
			has_failed = true;
			break;
		}

		printf("\t%-*s\t%-*s\t%s\t%d checks\n",
				max_suite_name_len, tests[test].suite,
				max_test_name_len, tests[test].name, state, tests[test].count);
	}

	return has_failed;
}

/**
 * Ensure the init call modifies the data structure
 */
static void test_afu_init() {
	test_start("AFU", "init");

	ocxl_afu afu1, afu2;

	memset(&afu1, 0xff, sizeof(afu1));
	memset(&afu2, 0xff, sizeof(afu2));

	ASSERT(!memcmp(&afu1, &afu2, sizeof(afu1)));
	afu_init(&afu1);
	ASSERT(memcmp(&afu1, &afu2, sizeof(afu1)));

	ocxl_afu_h afu = (ocxl_afu_h) &afu1;
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), ""));
	ASSERT(!strcmp(ocxl_afu_get_sysfs_path(afu), ""));
	ASSERT(ocxl_afu_get_fd(afu) == -1);
	ASSERT(ocxl_afu_get_global_mmio_size(afu) == 0);
	ASSERT(ocxl_afu_get_mmio_size(afu) == 0);

	test_stop(SUCCESS);

end:
	;
}

/**
 * Check that the AFU can allocated
 */
static void test_ocxl_afu_alloc() {
	test_start("AFU", "ocxl_afu_alloc");

	ocxl_afu template;
	afu_init(&template);

	ocxl_afu_h afu = 0;
	ASSERT(OCXL_OK == ocxl_afu_alloc(&afu));
	ASSERT(afu != 0);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), ""));
	ASSERT(!strcmp(ocxl_afu_get_sysfs_path(afu), ""));
	ASSERT(ocxl_afu_get_fd(afu) == -1);
	ASSERT(ocxl_afu_get_global_mmio_size(afu) == 0);
	ASSERT(ocxl_afu_get_mmio_size(afu) == 0);


	test_stop(SUCCESS);

end:
	ocxl_afu_free(afu);
}

/**
 * Check device_matches
 */
static void test_device_matches() {
	test_start("AFU", "device_matches");

	DIR *dev_dir = opendir("/dev");
	int dev_fd = dirfd(dev_dir);
	struct stat urandom;
	ASSERT(!fstatat(dev_fd, "urandom", &urandom, 0));
	ASSERT(!device_matches(dev_fd, "zero", urandom.st_rdev));
	ASSERT(device_matches(dev_fd, "urandom", urandom.st_rdev));

	test_stop(SUCCESS);

end:
	closedir(dev_dir);
}

/**
 * Check read_sysfs_uint
 */
static void test_read_sysfs_uint() {
	test_start("internal", "read_sysfs_uint");

	uint64_t val;

	ASSERT(!read_sysfs_uint("unittests/data/read_sysfs_uint/10", &val));
	ASSERT(val == 10);

	ASSERT(!read_sysfs_uint("unittests/data/read_sysfs_uint/100", &val));
	ASSERT(val == 100);

	ASSERT(!read_sysfs_uint("unittests/data/read_sysfs_uint/1000", &val));
	ASSERT(val == 1000);

	ASSERT(!read_sysfs_uint("unittests/data/read_sysfs_uint/uint64_max", &val));
	ASSERT(val == UINT64_MAX);

	ocxl_want_verbose_errors(0);
	ASSERT(read_sysfs_uint("unittests/data/read_sysfs_uint/negative", &val));

	test_stop(SUCCESS);

end:
	ocxl_want_verbose_errors(1);
}


pthread_t afu_thread = 0;
pthread_t usrirq_thread = 0;

/**
 * Create the virtual AFU
 * @post afu_thread is set and must be joined
 */
static void create_afu() {
	afu_thread = create_ocxl_device("IBM,Dummy", GLOBAL_MMIO_SIZE, PER_PASID_MMIO_SIZE);
	if (!afu_thread) {
		fprintf(stderr, "Could not create dummy AFU\n");
		exit(1);
	}

	usrirq_thread = create_usrirq_device("usrirq-test");
	if (!usrirq_thread) {
		fprintf(stderr, "Could not create dummy usrirq\n");
		exit(1);
	}
}

/**
 * check populate_metadata()
 */
static void test_populate_metadata() {
	test_start("AFU", "populate_metadata");

	struct stat dev_stat;
	ASSERT(!stat("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &dev_stat));

	ocxl_afu afu;
	afu_init(&afu);

	ASSERT(populate_metadata(dev_stat.st_rdev, &afu));
	ASSERT(!strcmp(afu.identifier.afu_name, "IBM,Dummy"));
	ASSERT(afu.identifier.afu_index == 0);
	ASSERT(!strcmp(afu.device_path, "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));
	ASSERT(!strcmp(afu.sysfs_path, "/tmp/ocxl-test/IBM,Dummy.0001:00:00.1.0"));
	ASSERT(afu.global_mmio.length == 32*1024*1024);
	ASSERT(afu.per_pasid_mmio.length == 16384);

	test_stop(SUCCESS);

end:
	;
}

/**
 * Check get_afu_by_path
 */
static void test_get_afu_by_path() {
	test_start("AFU", "get_afu_by_path");

	ocxl_afu_h afu = 0;
	const char *symlink_path = "/tmp/ocxl-test-symlink";

	ocxl_want_verbose_errors(0);
	ASSERT(OCXL_NO_DEV == get_afu_by_path("/nonexistent", &afu));
	ocxl_want_verbose_errors(1);
	ASSERT(0 == afu);

	afu = 0;
	ASSERT(OCXL_OK == get_afu_by_path("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ASSERT(afu != 0);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));
	ocxl_afu_free(afu);

	afu = 0;

	ASSERT(0 == symlink("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", symlink_path));

	ASSERT(OCXL_OK == get_afu_by_path(symlink_path, &afu));
	ASSERT(afu != 0);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));

	test_stop(SUCCESS);

end:
	ocxl_want_verbose_errors(1);
	if (afu) {
		ocxl_afu_free(afu);
	}

	unlink(symlink_path);
}

/**
 * Check ocxl_afu_open
 */
static void test_ocxl_afu_open() {
	test_start("AFU", "ocxl_afu_open");

	ocxl_afu_h afu = 0;

	ASSERT(OCXL_OK == get_afu_by_path("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu * my_afu = (ocxl_afu *)afu;
	ASSERT(my_afu->fd == -1);
	ASSERT(my_afu->irq_fd == -1);
	ASSERT(my_afu->epoll_fd == -1);

	ASSERT(OCXL_OK == ocxl_afu_open(afu));

	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->irq_fd != -1);
	ASSERT(my_afu->epoll_fd != -1);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_free(afu);
	}
}

/**
 * Check ocxl_afu_open_from_dev
 */
static void test_ocxl_afu_open_from_dev() {
	test_start("AFU", "ocxl_afu_open_from_dev");

	ocxl_afu_h afu = 0;

	ocxl_want_verbose_errors(0);
	ASSERT(OCXL_NO_DEV == ocxl_afu_open_from_dev("/nonexistent", &afu));
	ocxl_want_verbose_errors(1);

	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu * my_afu = (ocxl_afu *)afu;
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->irq_fd != -1);
	ASSERT(my_afu->epoll_fd != -1);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));

	test_stop(SUCCESS);

end:
	ocxl_want_verbose_errors(1);
	if (afu) {
		ocxl_afu_free(afu);
	}
}

#ifdef _ARCH_PPC64
/**
 * Check ocxl_afu_use_from_dev
 */
static void test_ocxl_afu_use_from_dev() {
	test_start("AFU", "ocxl_afu_open_from_dev");

	ocxl_afu_h afu = 0;

	ocxl_want_verbose_errors(0);
	ASSERT(OCXL_NO_DEV == ocxl_afu_use_from_dev("/nonexistent", &afu, 0, OCXL_MMIO_HOST_ENDIAN, OCXL_MMIO_HOST_ENDIAN));
	ocxl_want_verbose_errors(1);

	ASSERT(OCXL_OK == ocxl_afu_use_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu, 0, OCXL_MMIO_HOST_ENDIAN, OCXL_MMIO_HOST_ENDIAN));
	ocxl_afu * my_afu = (ocxl_afu *)afu;
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->irq_fd != -1);
	ASSERT(my_afu->epoll_fd != -1);
	ASSERT(my_afu->global_mmio.start != NULL);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));

	test_stop(SUCCESS);

end:
ocxl_want_verbose_errors(1);
	if (afu) {
		ocxl_afu_free(afu);
	}
}
#endif


/**
 * Check ocxl_afu_attach
 */
static void test_ocxl_afu_attach() {
	test_start("AFU", "ocxl_afu_attach");

	ocxl_afu_h afu = 0;

	ASSERT(!afu_is_attached());
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ASSERT(OCXL_OK == ocxl_afu_attach(afu));
	ASSERT(afu_is_attached());

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_free(afu);
	}
}

/**
 * Check ocxl_afu_close
 */
static void test_ocxl_afu_close_free() {
	test_start("AFU", "ocxl_afu_close/free");

	ocxl_afu_h afu = 0;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ASSERT(OCXL_OK == ocxl_afu_close(afu));
	ASSERT(afu != 0);
	ocxl_afu * my_afu = (ocxl_afu *)afu;
	ASSERT(-1 == my_afu->fd);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_free(afu);
	}
}

/**
 * Check ocxl_global_mmio_map/unmap
 */
static void test_ocxl_global_mmio_map() {
	test_start("MMIO", "ocxl_global_mmio_map/unmap");

	ocxl_afu_h afu = 0;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu * my_afu = (ocxl_afu *)afu;

	ASSERT(my_afu->global_mmio_fd == -1);
	ASSERT(my_afu->global_mmio.start == NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);

	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_HOST_ENDIAN));
	ASSERT(my_afu->global_mmio_fd != -1);
	ASSERT(my_afu->global_mmio.start != NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);
	ASSERT(my_afu->global_mmio.endianess == OCXL_MMIO_HOST_ENDIAN);

	ocxl_global_mmio_unmap(afu);
	ASSERT(my_afu->global_mmio_fd == -1);
	ASSERT(my_afu->global_mmio.start == NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);

	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_BIG_ENDIAN));
	ASSERT(my_afu->global_mmio_fd != -1);
	ASSERT(my_afu->global_mmio.start != NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);
	ASSERT(my_afu->global_mmio.endianess == OCXL_MMIO_BIG_ENDIAN);

	ocxl_global_mmio_unmap(afu);
	ASSERT(my_afu->global_mmio_fd == -1);
	ASSERT(my_afu->global_mmio.start == NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);

	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_LITTLE_ENDIAN));
	ASSERT(my_afu->global_mmio_fd != -1);
	ASSERT(my_afu->global_mmio.start != NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);
	ASSERT(my_afu->global_mmio.endianess == OCXL_MMIO_LITTLE_ENDIAN);

	ASSERT(OCXL_OK == ocxl_afu_close(afu));
	ASSERT(my_afu->global_mmio_fd == -1);
	ASSERT(my_afu->global_mmio.start == NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_free(afu);
	}
}

#ifdef __UNUSED
/**
 * Check ocxl_mmio_map/unmap
 */
static void test_ocxl_mmio_map() {
	test_start("MMIO", "ocxl_mmio_map/unmap");

	ocxl_afu_h afu = 0;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu * my_afu = (ocxl_afu *)afu;

	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->per_pasid_mmio.start == NULL);
	ASSERT(my_afu->per_pasid_mmio.length == PER_PASID_MMIO_SIZE);

	ASSERT(OCXL_OK == ocxl_mmio_map(afu, OCXL_MMIO_HOST_ENDIAN));
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->per_pasid_mmio.start != NULL);
	ASSERT(my_afu->per_pasid_mmio.length == PER_PASID_MMIO_SIZE);
	ASSERT(my_afu->per_pasid_mmio.endianess == OCXL_MMIO_HOST_ENDIAN);

	ocxl_mmio_unmap(afu);
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->per_pasid_mmio.start == NULL);
	ASSERT(my_afu->per_pasid_mmio.length == PER_PASID_MMIO_SIZE);

	ASSERT(OCXL_OK == ocxl_mmio_map(afu, OCXL_MMIO_BIG_ENDIAN));
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->per_pasid_mmio.start != NULL);
	ASSERT(my_afu->per_pasid_mmio.length == PER_PASID_MMIO_SIZE);
	ASSERT(my_afu->per_pasid_mmio.endianess == OCXL_MMIO_BIG_ENDIAN);

	ocxl_mmio_unmap(afu);
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->per_pasid_mmio.start == NULL);
	ASSERT(my_afu->per_pasid_mmio.length == PER_PASID_MMIO_SIZE);

	ASSERT(OCXL_OK == ocxl_mmio_map(afu, OCXL_MMIO_LITTLE_ENDIAN));
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->per_pasid_mmio.start != NULL);
	ASSERT(my_afu->per_pasid_mmio.length == PER_PASID_MMIO_SIZE);
	ASSERT(my_afu->per_pasid_mmio.endianess == OCXL_MMIO_LITTLE_ENDIAN);

	ASSERT(OCXL_OK == ocxl_afu_close(afu));
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->per_pasid_mmio.start == NULL);
	ASSERT(my_afu->per_pasid_mmio.length == PER_PASID_MMIO_SIZE);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_free(afu);
	}
}
#endif

/**
 * Check mmio_check
 */
static void test_mmio_check() {
	test_start("MMIO", "mmio_check");

	ocxl_afu_h afu = 0;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu * my_afu = (ocxl_afu *)afu;

	ASSERT(my_afu->global_mmio_fd == -1);
	ASSERT(my_afu->global_mmio.start == NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);

	ocxl_want_verbose_errors(0);
	ASSERT(OCXL_NO_CONTEXT == mmio_check(my_afu, true, 0, 4));
	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_HOST_ENDIAN));
	ASSERT(OCXL_OUT_OF_BOUNDS == mmio_check(my_afu, true, GLOBAL_MMIO_SIZE + 8, 4));
	ocxl_want_verbose_errors(1);

	ASSERT(OCXL_OK == mmio_check(my_afu, true, 0, 4));

	test_stop(SUCCESS);

end:
	ocxl_want_verbose_errors(1);
	if (afu) {
		ocxl_afu_free(afu);
	}
}

/**
 * Check read32/write32
 */
static void test_read32() {
	test_start("MMIO", "mmio_read32/write32");

	ocxl_afu_h afu = 0;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu * my_afu = (ocxl_afu *)afu;
	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_HOST_ENDIAN));

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		write32(&my_afu->global_mmio, offset, offset);
	}

	bool good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		if (read32(&my_afu->global_mmio, offset) != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	ocxl_global_mmio_unmap(afu);

	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_LITTLE_ENDIAN));
	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		write32(&my_afu->global_mmio, offset, offset);
	}

	good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		if (read32(&my_afu->global_mmio, offset) != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	uint32_t little = *(uint32_t *)(my_afu->global_mmio.start + 4);

	ASSERT(4 == le32toh(little));

	ocxl_global_mmio_unmap(afu);

	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_BIG_ENDIAN));
	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		write32(&my_afu->global_mmio, offset, offset);
	}

	good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		if (read32(&my_afu->global_mmio, offset) != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	uint32_t big = *(uint32_t *)(my_afu->global_mmio.start + 4);
	ASSERT(4 == be32toh(big));
	ASSERT(big != little);


	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_free(afu);
	}
}

/**
 * Check read64/write64
 */
static void test_read64() {
	test_start("MMIO", "mmio_read64/write64");

	ocxl_afu_h afu = 0;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu * my_afu = (ocxl_afu *)afu;
	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_HOST_ENDIAN));

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		write64(&my_afu->global_mmio, offset, offset);
	}

	bool good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		if (read64(&my_afu->global_mmio, offset) != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	ocxl_global_mmio_unmap(afu);

	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_LITTLE_ENDIAN));
	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		write64(&my_afu->global_mmio, offset, offset);
	}

	good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		if (read64(&my_afu->global_mmio, offset) != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	uint64_t little = *(uint64_t *)(my_afu->global_mmio.start + 8);

	ASSERT(8 == le64toh(little));

	ocxl_global_mmio_unmap(afu);

	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_BIG_ENDIAN));
	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		write64(&my_afu->global_mmio, offset, offset);
	}

	good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		if (read64(&my_afu->global_mmio, offset) != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	uint64_t big = *(uint64_t *)(my_afu->global_mmio.start + 8);
	ASSERT(be64toh(big) == 8);
	ASSERT(big != little);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_free(afu);
	}
}

/**
 * Check ocxl_global_mmio_read32/write32
 */
static void test_ocxl_global_mmio_read32() {
	test_start("MMIO", "ocxl_global_mmio_read32/write32");

	ocxl_afu_h afu = 0;
	uint32_t val;

	ocxl_want_verbose_errors(0);

	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ASSERT(OCXL_NO_CONTEXT == ocxl_global_mmio_write32(afu, 0, 0));
	ASSERT(OCXL_NO_CONTEXT == ocxl_global_mmio_read32(afu, 0, &val));

	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_HOST_ENDIAN));

	ASSERT(OCXL_OUT_OF_BOUNDS == ocxl_global_mmio_write32(afu, GLOBAL_MMIO_SIZE + 4, 0));
	ASSERT(OCXL_OUT_OF_BOUNDS == ocxl_global_mmio_read32(afu, GLOBAL_MMIO_SIZE + 4, &val));

	// Already did rigorous tests for read/write32, so just do a quick test for the wrappers
	ocxl_want_verbose_errors(1);
	ASSERT(OCXL_OK == ocxl_global_mmio_write32(afu, 0, 12345678));
	ASSERT(OCXL_OK == ocxl_global_mmio_read32(afu, 0, &val));
	ASSERT(val == 12345678);

	test_stop(SUCCESS);

end:
	ocxl_want_verbose_errors(1);
	if (afu) {
		ocxl_afu_free(afu);
	}
}

/**
 * Check ocxl_global_mmio_read64/write64
 */
static void test_ocxl_global_mmio_read64() {
	test_start("MMIO", "ocxl_global_mmio_read64/write64");

	ocxl_afu_h afu = 0;
	uint64_t val;

	ocxl_want_verbose_errors(0);

	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ASSERT(OCXL_NO_CONTEXT == ocxl_global_mmio_write64(afu, 0, 0));
	ASSERT(OCXL_NO_CONTEXT == ocxl_global_mmio_read64(afu, 0, &val));

	ASSERT(OCXL_OK == ocxl_global_mmio_map(afu, OCXL_MMIO_HOST_ENDIAN));

	ASSERT(OCXL_OUT_OF_BOUNDS == ocxl_global_mmio_write64(afu, GLOBAL_MMIO_SIZE + 8, 0));
	ASSERT(OCXL_OUT_OF_BOUNDS == ocxl_global_mmio_read64(afu, GLOBAL_MMIO_SIZE + 8, &val));

	// Already did rigorous tests for read/write64, so just do a quick test for the wrappers
	ocxl_want_verbose_errors(1);
	ASSERT(OCXL_OK == ocxl_global_mmio_write64(afu, 0, 12345678));
	ASSERT(OCXL_OK == ocxl_global_mmio_read64(afu, 0, &val));
	ASSERT(val == 12345678);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_free(afu);
	}
}

static void exit_handler() {
	void *ret;

	if (afu_thread) {
		stop_afu();
		pthread_kill(afu_thread, SIGTERM);
		pthread_join(afu_thread, &ret);
		term_afu();
	}

	if (usrirq_thread) {
		stop_usrirq();
		pthread_kill(usrirq_thread, SIGTERM);
		pthread_join(usrirq_thread, &ret);
		term_usrirq();
	}
}

int main(int args, const char **argv) {
	struct sigaction sa;
	sa.sa_handler = exit_handler;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void)sigaction(SIGINT, &sa, NULL);

	ocxl_set_sys_path(ocxl_sysfs_path);
	ocxl_set_dev_path(ocxl_dev_path);
	ocxl_set_irq_path(ocxl_irq_path);
	ocxl_want_verbose_errors(1);

	struct stat sysfs_stat;
	if (stat(ocxl_sysfs_path, &sysfs_stat)) {
		if (mkdir(ocxl_sysfs_path, 0775)) {
			fprintf(stderr, "Could not mkdir '%s': %d: %s\n",
					ocxl_sysfs_path, errno, strerror(errno));
		}
	}

	test_read_sysfs_uint();
	test_afu_init();
	test_ocxl_afu_alloc();
	test_device_matches();

	create_afu();
	sleep(1);

	test_populate_metadata();
	test_get_afu_by_path();
	test_ocxl_afu_open();
	test_ocxl_afu_open_from_dev();
	test_ocxl_afu_attach();
	test_ocxl_afu_close_free();
#ifdef _ARCH_PPC64
	test_ocxl_afu_use_from_dev();
#endif

	test_ocxl_global_mmio_map();
	// Disabled as we need MMAP support in CUSE to test this
	// test_ocxl_mmio_map();
	test_mmio_check();
	test_read32();
	test_read64();
	test_ocxl_global_mmio_read32();
	test_ocxl_global_mmio_read64();

	exit_handler();

	if (test_report()) {
		return 1;
	}

	return 0;
}