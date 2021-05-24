/* Copyright (c) 2013-2018, Linaro Limited
 * Copyright (c) 2020, Nokia
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

/*
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#include <odp_posix_extensions.h>

#include <odp/api/system_info.h>
#include <odp/api/version.h>
#include <odp_global_data.h>
#include <odp_sysinfo_internal.h>
#include <odp_init_internal.h>
#include <odp_libconfig_internal.h>
#include <odp_debug_internal.h>
#include <odp_config_internal.h>
#include <odp/api/align.h>
#include <odp/api/cpu.h>
#include <odp_packet_internal.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>

#include <rte_string_fns.h>
#include <rte_version.h>

/* sysconf */
#include <unistd.h>
#include <sys/sysinfo.h>

/* opendir, readdir */
#include <sys/types.h>
#include <dirent.h>

#define CACHE_LNSZ_FILE \
	"/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size"

/*
 * Report the number of logical CPUs detected at boot time
 */
static int sysconf_cpu_count(void)
{
	return odp_global_ro.num_cpus_installed;
}

#if defined __x86_64__ || defined __i386__ || defined __OCTEON__ || defined __powerpc__
/*
 * Analysis of /sys/devices/system/cpu/ files
 */
static int systemcpu_cache_line_size(void)
{
	FILE  *file;
	char str[128];
	int size = 0;

	file = fopen(CACHE_LNSZ_FILE, "rt");
	if (file == NULL) {
		/* File not found */
		return 0;
	}

	if (fgets(str, sizeof(str), file) != NULL) {
		/* Read cache line size */
		if (sscanf(str, "%i", &size) != 1)
			size = 0;
	}

	fclose(file);

	return size;
}

#else
/*
 * Use dummy data if not available from /sys/devices/system/cpu/
 */
static int systemcpu_cache_line_size(void)
{
	return 64;
}
#endif

static uint64_t default_huge_page_size(void)
{
	char str[1024];
	unsigned long sz;
	FILE *file;

	file = fopen("/proc/meminfo", "rt");
	if (!file)
		return 0;

	while (fgets(str, sizeof(str), file) != NULL) {
		if (sscanf(str, "Hugepagesize:   %8lu kB", &sz) == 1) {
			ODP_DBG("defaut hp size is %" PRIu64 " kB\n", sz);
			fclose(file);
			return (uint64_t)sz * 1024;
		}
	}

	ODP_ERR("unable to get default hp size\n");
	fclose(file);
	return 0;
}

/*
 * returns a malloced string containing the name of the directory for
 * huge pages of a given size (0 for default)
 * largely "inspired" by dpdk:
 * lib/librte_eal/linuxapp/eal/eal_hugepage_info.c: get_hugepage_dir
 *
 * Analysis of /proc/mounts
 */
static char *get_hugepage_dir(uint64_t hugepage_sz)
{
	enum proc_mount_fieldnames {
		DEVICE = 0,
		MOUNTPT,
		FSTYPE,
		OPTIONS,
		_FIELDNAME_MAX
	};
	static uint64_t default_size;
	const char *proc_mounts = "/proc/mounts";
	const char *hugetlbfs_str = "hugetlbfs";
	const size_t htlbfs_str_len = sizeof(hugetlbfs_str) - 1;
	const char *pagesize_opt = "pagesize=";
	const size_t pagesize_opt_len = sizeof(pagesize_opt) - 1;
	const char split_tok = ' ';
	char *tokens[_FIELDNAME_MAX];
	char buf[BUFSIZ];
	char *retval = NULL;
	const char *pagesz_str;
	uint64_t pagesz;
	FILE *fd = fopen(proc_mounts, "r");

	if (fd == NULL)
		return NULL;

	if (default_size == 0)
		default_size = default_huge_page_size();

	if (hugepage_sz == 0)
		hugepage_sz = default_size;

	while (fgets(buf, sizeof(buf), fd)) {
		if (rte_strsplit(buf, sizeof(buf), tokens,
				 _FIELDNAME_MAX, split_tok) != _FIELDNAME_MAX) {
			ODP_ERR("Error parsing %s\n", proc_mounts);
			break; /* return NULL */
		}

		/* is this hugetlbfs? */
		if (!strncmp(tokens[FSTYPE], hugetlbfs_str, htlbfs_str_len)) {
			pagesz_str = strstr(tokens[OPTIONS], pagesize_opt);

			/* No explicit size, default page size is compared */
			if (pagesz_str == NULL) {
				if (hugepage_sz == default_size) {
					retval = strdup(tokens[MOUNTPT]);
					break;
				}
			} else {
				/* there is an explicit page size, so check it */
				pagesz = rte_str_to_size(&pagesz_str[pagesize_opt_len]);
				if (pagesz == hugepage_sz) {
					retval = strdup(tokens[MOUNTPT]);
					break;
				}
			}
		} /* end if strncmp hugetlbfs */
	} /* end while fgets */

	fclose(fd);
	return retval;
}

/*
 * Analysis of /sys/devices/system/cpu/cpu%d/cpufreq/ files
 */
static uint64_t read_cpufreq(const char *filename, int id)
{
	char path[256], buffer[256], *endptr = NULL;
	FILE *file;
	uint64_t ret = 0;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/%s", id, filename);

	file = fopen(path, "r");
	if (file == NULL)
		return ret;

	if (fgets(buffer, sizeof(buffer), file) != NULL)
		ret = strtoull(buffer, &endptr, 0) * 1000;

	fclose(file);

	return ret;
}

/*
 * Analysis of /sys/devices/system/cpu/ files
 */
static int systemcpu(system_info_t *sysinfo)
{
	int ret;

	ret = sysconf_cpu_count();
	if (ret == 0) {
		ODP_ERR("sysconf_cpu_count failed.\n");
		return -1;
	}

	sysinfo->cpu_count = ret;

	ret = systemcpu_cache_line_size();
	if (ret == 0) {
		ODP_ERR("systemcpu_cache_line_size failed.\n");
		return -1;
	}

	sysinfo->cache_line_size = ret;

	if (ret != ODP_CACHE_LINE_SIZE) {
		ODP_ERR("Cache line sizes definitions don't match.\n");
		return -1;
	}

	return 0;
}

/*
 * Huge page information
 */
static int system_hp(hugepage_info_t *hugeinfo)
{
	hugeinfo->default_huge_page_size = default_huge_page_size();

	/* default_huge_page_dir may be NULL if no huge page support */
	hugeinfo->default_huge_page_dir = get_hugepage_dir(0);

	return 0;
}

static int read_config_file(void)
{
	const char *str;
	int val = 0;

	str = "system.cpu_mhz";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}
	odp_global_ro.system_info.default_cpu_hz = (uint64_t)val * 1000000;

	str = "system.cpu_mhz_max";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}
	odp_global_ro.system_info.default_cpu_hz_max = (uint64_t)val * 1000000;

	return 0;
}

/*
 * System info initialisation
 */
int _odp_system_info_init(void)
{
	int num_cpus;
	int i;
	FILE  *file;

	memset(&odp_global_ro.system_info, 0, sizeof(system_info_t));

	odp_global_ro.system_info.page_size = ODP_PAGE_SIZE;

	/* Read default CPU Hz values from config file */
	if (read_config_file())
		return -1;

	/* Check that CONFIG_NUM_CPU_IDS is large enough */
	num_cpus = get_nprocs_conf();
	if (num_cpus > CONFIG_NUM_CPU_IDS)
		ODP_ERR("Unable to handle all %d "
			"CPU IDs. Increase CONFIG_NUM_CPU_IDS value.\n",
			num_cpus);

	/* By default, read max frequency from a cpufreq file */
	for (i = 0; i < CONFIG_NUM_CPU_IDS; i++) {
		uint64_t cpu_hz_max = read_cpufreq("cpuinfo_max_freq", i);

		if (cpu_hz_max)
			odp_global_ro.system_info.cpu_hz_max[i] = cpu_hz_max;
	}

	file = fopen("/proc/cpuinfo", "rt");
	if (file != NULL) {
		/* Read CPU model, and set max cpu frequency
		 * if not set from cpufreq. */
		_odp_cpuinfo_parser(file, &odp_global_ro.system_info);
		fclose(file);
	} else {
		_odp_dummy_cpuinfo(&odp_global_ro.system_info);
	}

	if (systemcpu(&odp_global_ro.system_info)) {
		ODP_ERR("systemcpu failed\n");
		return -1;
	}

	system_hp(&odp_global_ro.hugepage_info);

	return 0;
}

/*
 * System info termination
 */
int _odp_system_info_term(void)
{
	free(odp_global_ro.hugepage_info.default_huge_page_dir);

	return 0;
}

/*
 *************************
 * Public access functions
 *************************
 */
uint64_t odp_cpu_hz_current(int id)
{
	uint64_t cur_hz = read_cpufreq("cpuinfo_cur_freq", id);

	if (!cur_hz)
		cur_hz = odp_cpu_arch_hz_current(id);

	return cur_hz;
}

uint64_t odp_cpu_hz(void)
{
	int id = sched_getcpu();

	return odp_cpu_hz_current(id);
}

uint64_t odp_cpu_hz_id(int id)
{
	return odp_cpu_hz_current(id);
}

uint64_t odp_cpu_hz_max(void)
{
	return odp_cpu_hz_max_id(0);
}

uint64_t odp_cpu_hz_max_id(int id)
{
	if (id >= 0 && id < CONFIG_NUM_CPU_IDS)
		return odp_global_ro.system_info.cpu_hz_max[id];
	else
		return 0;
}

uint64_t odp_sys_huge_page_size(void)
{
	return odp_global_ro.hugepage_info.default_huge_page_size;
}

static int pagesz_compare(const void *pagesz1, const void *pagesz2)
{
	return (*(const uint64_t *)pagesz1 - *(const uint64_t *)pagesz2);
}

int odp_sys_huge_page_size_all(uint64_t size[], int num)
{
	DIR *dir;
	struct dirent *entry;
	int pagesz_num = 0;
	int saved = 0;

	/* See: kernel.org: hugetlbpage.txt */
	dir = opendir("/sys/kernel/mm/hugepages");
	if (!dir) {
		ODP_PRINT("Failed to open /sys/kernel/mm/hugepages: %s\n",
			  strerror(errno));
		return 0;
	}

	while ((entry = readdir(dir)) != NULL) {
		unsigned long sz;

		if (sscanf(entry->d_name, "hugepages-%8lukB", &sz) == 1) {
			if (size != NULL && saved < num)
				size[saved++] = sz * 1024;
			pagesz_num++;
		}
	}
	closedir(dir);

	if (size != NULL && saved > 1)
		qsort(size, saved, sizeof(uint64_t), pagesz_compare);

	return pagesz_num;
}

uint64_t odp_sys_page_size(void)
{
	return odp_global_ro.system_info.page_size;
}

const char *odp_cpu_model_str(void)
{
	return odp_cpu_model_str_id(0);
}

const char *odp_cpu_model_str_id(int id)
{
	if (id >= 0 && id < CONFIG_NUM_CPU_IDS)
		return odp_global_ro.system_info.model_str[id];
	else
		return NULL;
}

int odp_sys_cache_line_size(void)
{
	return odp_global_ro.system_info.cache_line_size;
}

int odp_cpu_count(void)
{
	return odp_global_ro.system_info.cpu_count;
}

int odp_system_info(odp_system_info_t *info)
{
	system_info_t *sys_info = &odp_global_ro.system_info;

	memset(info, 0, sizeof(odp_system_info_t));

	info->cpu_arch   = sys_info->cpu_arch;
	info->cpu_isa_sw = sys_info->cpu_isa_sw;
	info->cpu_isa_hw = sys_info->cpu_isa_hw;

	return 0;
}

void odp_sys_info_print(void)
{
	int len, num_cpu;
	int max_len = 512;
	odp_cpumask_t cpumask;
	char cpumask_str[ODP_CPUMASK_STR_SIZE];
	char str[max_len];

	memset(cpumask_str, 0, sizeof(cpumask_str));

	num_cpu = odp_cpumask_all_available(&cpumask);
	odp_cpumask_to_str(&cpumask, cpumask_str, ODP_CPUMASK_STR_SIZE);

	len = snprintf(str, max_len, "\n"
		       "ODP system info\n"
		       "---------------\n"
		       "ODP API version:  %s\n"
		       "ODP impl name:    %s\n"
		       "ODP impl details: %s\n"
		       "DPDK version:     %d.%d.%d\n"
		       "CPU model:        %s\n"
		       "CPU freq (hz):    %" PRIu64 "\n"
		       "Cache line size:  %i\n"
		       "CPU count:        %i\n"
		       "CPU mask:         %s\n"
		       "\n",
		       odp_version_api_str(),
		       odp_version_impl_name(),
		       odp_version_impl_str(),
		       RTE_VER_YEAR, RTE_VER_MONTH, RTE_VER_MINOR,
		       odp_cpu_model_str(),
		       odp_cpu_hz_max(),
		       odp_sys_cache_line_size(),
		       num_cpu, cpumask_str);

	str[len] = '\0';
	ODP_PRINT("%s", str);

	_odp_sys_info_print_arch();
}

void odp_sys_config_print(void)
{
	/* Print ODP_CONFIG_FILE default and override values */
	if (_odp_libconfig_print())
		ODP_ERR("Config file print failed\n");

	ODP_PRINT("\n\nodp_config_internal.h values:\n"
		  "-----------------------------\n");
	ODP_PRINT("CONFIG_NUM_CPU_IDS:          %i\n", CONFIG_NUM_CPU_IDS);
	ODP_PRINT("ODP_CONFIG_POOLS:            %i\n", ODP_CONFIG_POOLS);
	ODP_PRINT("CONFIG_INTERNAL_QUEUES:      %i\n", CONFIG_INTERNAL_QUEUES);
	ODP_PRINT("CONFIG_MAX_PLAIN_QUEUES:     %i\n", CONFIG_MAX_PLAIN_QUEUES);
	ODP_PRINT("CONFIG_MAX_SCHED_QUEUES:     %i\n", CONFIG_MAX_SCHED_QUEUES);
	ODP_PRINT("CONFIG_MAX_QUEUES:           %i\n", CONFIG_MAX_QUEUES);
	ODP_PRINT("CONFIG_QUEUE_MAX_ORD_LOCKS:  %i\n", CONFIG_QUEUE_MAX_ORD_LOCKS);
	ODP_PRINT("ODP_CONFIG_PKTIO_ENTRIES:    %i\n", ODP_CONFIG_PKTIO_ENTRIES);
	ODP_PRINT("ODP_CONFIG_BUFFER_ALIGN_MIN: %i\n", ODP_CONFIG_BUFFER_ALIGN_MIN);
	ODP_PRINT("ODP_CONFIG_BUFFER_ALIGN_MAX: %i\n", ODP_CONFIG_BUFFER_ALIGN_MAX);
	ODP_PRINT("CONFIG_PACKET_HEADROOM:      %i\n", CONFIG_PACKET_HEADROOM);
	ODP_PRINT("CONFIG_PACKET_TAILROOM:      %i\n", CONFIG_PACKET_TAILROOM);
	ODP_PRINT("CONFIG_PACKET_SEG_SIZE:      %i\n", CONFIG_PACKET_SEG_SIZE);
	ODP_PRINT("CONFIG_PACKET_SEG_LEN_MIN:   %i\n", CONFIG_PACKET_SEG_LEN_MIN);
	ODP_PRINT("CONFIG_PACKET_MAX_SEG_LEN:   %i\n", CONFIG_PACKET_MAX_SEG_LEN);
	ODP_PRINT("ODP_CONFIG_SHM_BLOCKS:       %i\n", ODP_CONFIG_SHM_BLOCKS);
	ODP_PRINT("CONFIG_BURST_SIZE:           %i\n", CONFIG_BURST_SIZE);
	ODP_PRINT("CONFIG_POOL_MAX_NUM:         %i\n", CONFIG_POOL_MAX_NUM);
	ODP_PRINT("\n");
}
