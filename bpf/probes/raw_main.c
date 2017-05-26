/*
 *  Copyright (C) 2017 Authors of Cilium
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <sys/resource.h>

#include "raw_insn.h"

#define BPF_MAX_FIXUPS	64

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

struct bpf_map_fixup {
	int off;
	enum bpf_map_type type;
	uint32_t size_key;
	uint32_t size_val;
	uint32_t flags;
};

struct bpf_test {
	const char *emits;
	enum bpf_prog_type type;
	struct bpf_insn insns[BPF_MAXINSNS];
	struct bpf_map_fixup fixup_map[BPF_MAX_FIXUPS];
	const char *warn;
};

static struct bpf_test tests[] = {
#include "raw_probe.t"
};

static uint64_t bpf_ptr_to_u64(const void *ptr)
{
	return (uint64_t)(unsigned long)ptr;
}

#ifndef __NR_bpf
# if defined(__i386__)
#  define __NR_bpf 357
# elif defined(__x86_64__)
#  define __NR_bpf 321
# elif defined(__aarch64__)
#  define __NR_bpf 280
# else
#  warning __NR_bpf not defined.
# endif
#endif

static int bpf(int cmd, union bpf_attr *attr, unsigned int size)
{
#ifdef __NR_bpf
	return syscall(__NR_bpf, cmd, attr, size);
#else
	errno = ENOSYS;
	return -1;
#endif
}

int bpf_prog_load(enum bpf_prog_type type, const struct bpf_insn *insns,
		  size_t num_insns, const char *license, char *log,
		  size_t size_log)
{
	union bpf_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.prog_type = type;
	attr.insns = bpf_ptr_to_u64(insns);
	attr.insn_cnt = num_insns;
	attr.license = bpf_ptr_to_u64(license);

	if (size_log > 0) {
		attr.log_buf = bpf_ptr_to_u64(log);
		attr.log_size = size_log;
		attr.log_level = 1;
	}

	return bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
}

static int bpf_map_create(enum bpf_map_type type, uint32_t size_key,
			  uint32_t size_value, uint32_t max_elem,
			  uint32_t flags)
{
	union bpf_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.map_type = type;
	attr.key_size = size_key;
	attr.value_size = size_value;
	attr.max_entries = max_elem;
	attr.map_flags = flags;

	return bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
}

static int bpf_test_length(const struct bpf_insn *insn)
{
	int len;

	for (len = BPF_MAXINSNS - 1; len > 0; --len)
		if (insn[len].code != 0 || insn[len].imm != 0)
			break;

	return len + 1;
}

static void bpf_report(const struct bpf_test *test, int success,
		       int debug_mode)
{
	static char bpf_vlog[1U << 16];
	int fd;

	printf("%s#define %s\n\n", success ? "" : "// ", test->emits);

	if (!success || debug_mode) {
		printf("#if 0\n");
		printf("%s %s: ", test->emits, success ?
		       "debug output" : "failed due to load error");

		memset(bpf_vlog, 0, sizeof(bpf_vlog));
		fd = bpf_prog_load(test->type, test->insns,
				   bpf_test_length(test->insns), "GPL",
				   bpf_vlog, sizeof(bpf_vlog));
		printf("%s\n%s", strerror(errno), bpf_vlog);
		printf("#endif\n\n");
		if (fd > 0)
			close(fd);
	}

	if (!success && test->warn)
		fprintf(stderr, "%s: %s\n", test->emits, test->warn);
}

static void bpf_run_test(struct bpf_test *test, int debug_mode)
{
	struct bpf_map_fixup *map = test->fixup_map;
	int fd;

	/* We can use off here as it's never first insns. */
	while (map->off) {
		fd = bpf_map_create(map->type, map->size_key,
				    map->size_val, 1, map->flags);
		if (fd < 0) {
			if (debug_mode) {
				printf("#if 0\n");
				printf("%s: bpf_map_create(): %s\n",
				       test->emits, strerror(errno));
				printf("#endif\n\n");
			}
			/* We fail in verifier eventually. */
			break;
		}
		test->insns[map->off].imm = fd;
		map++;
	}

	fd = bpf_prog_load(test->type, test->insns,
			   bpf_test_length(test->insns), "GPL", NULL, 0);
	bpf_report(test, fd > 0, debug_mode);
	if (fd > 0)
		close(fd);
}

int main(int argc, char **argv)
{
	struct rlimit rold, rinf = { RLIM_INFINITY, RLIM_INFINITY };
	int debug_mode = 0;
	int i;

	if (argc > 1 && !strncmp(argv[argc - 1], "debug", sizeof("debug")))
		debug_mode = 1;

	getrlimit(RLIMIT_MEMLOCK, &rold);
	setrlimit(RLIMIT_MEMLOCK, &rinf);

	for (i = 0; i < ARRAY_SIZE(tests); i++)
		bpf_run_test(&tests[i], debug_mode);

	setrlimit(RLIMIT_MEMLOCK, &rold);
	return 0;
}
