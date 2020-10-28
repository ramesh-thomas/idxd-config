/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2015-2019 Intel Corporation. All rights reserved. */
#include <limits.h>
#include <string.h>
#include <util/json.h>
#include <util/filter.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <uuid/uuid.h>
#include <json-c/json.h>
#include <json-c/printbuf.h>
#include <ccan/array_size/array_size.h>
#include <ccan/short_types/short_types.h>
#include <accfg.h>
#include <accfg/libaccel_config.h>
#include <dirent.h>
#include "sysfs.h"

static const char *wq_type_str[] = {
	"none",
	"kernel",
	"user",
	"mdev"
};

/* adapted from mdadm::human_size_brief() */
static int display_size(struct json_object *jobj, struct printbuf *pbuf,
			int level, int flags)
{
	unsigned long long bytes = json_object_get_int64(jobj);
	static char buf[128];
	int c;

	/*
	 * We convert bytes to either centi-M{ega,ibi}bytes or
	 * centi-G{igi,ibi}bytes, with appropriate rounding, and then print
	 * 1/100th of those as a decimal.  We allow upto 2048Megabytes before
	 * converting to gigabytes, as that shows more precision and isn't too
	 * large a number.  Terabytes are not yet handled.
	 *
	 * If prefix == IEC, we mean prefixes like kibi,mebi,gibi etc.
	 * If prefix == JEDEC, we mean prefixes like kilo,mega,giga etc.
	 */

	if (bytes < 5000 * 1024)
		snprintf(buf, sizeof(buf), "%lld", bytes);
	else {
		/* IEC */
		if (bytes < 2 * 1024LL * 1024LL * 1024LL) {
			long cMiB = (bytes * 200LL / (1LL << 20) + 1) / 2;

			c = snprintf(buf, sizeof(buf), "\"%ld.%02ld MiB",
				     cMiB / 100, cMiB % 100);
		} else {
			long cGiB = (bytes * 200LL / (1LL << 30) + 1) / 2;

			c = snprintf(buf, sizeof(buf), "\"%ld.%02ld GiB",
				     cGiB / 100, cGiB % 100);
		}

		/* JEDEC */
		if (bytes < 2 * 1024LL * 1024LL * 1024LL) {
			long cMB = (bytes / (1000000LL / 200LL) + 1) / 2;

			snprintf(buf + c, sizeof(buf) - c, " (%ld.%02ld MB)\"",
				 cMB / 100, cMB % 100);
		} else {
			long cGB = (bytes / (1000000000LL / 200LL) + 1) / 2;

			snprintf(buf + c, sizeof(buf) - c, " (%ld.%02ld GB)\"",
				 cGB / 100, cGB % 100);
		}
	}

	return printbuf_memappend(pbuf, buf, strlen(buf));
}

static int display_hex(struct json_object *jobj, struct printbuf *pbuf,
		       int level, int flags)
{
	unsigned long long val = json_object_get_int64(jobj);
	static char buf[32];
	snprintf(buf, sizeof(buf), "\"%#llx\"", val);
	return printbuf_memappend(pbuf, buf, strlen(buf));
}

struct json_object *util_json_object_size(unsigned long long size,
					  unsigned long flags)
{
	struct json_object *jobj = json_object_new_int64(size);

	if (jobj && (flags & UTIL_JSON_HUMAN))
		json_object_set_serializer(jobj, display_size, NULL, NULL);
	return jobj;
}

struct json_object *util_json_object_hex(unsigned long long val,
					 unsigned long flags)
{
	struct json_object *jobj = json_object_new_int64(val);

	if (jobj)
		json_object_set_serializer(jobj, display_hex, NULL, NULL);
	return jobj;
}

/* API used to output json object display to console */
void util_display_json_array(FILE * f_out, struct json_object *jarray,
			     unsigned long flags)
{
	int len = json_object_array_length(jarray);
	int jflag = JSON_C_TO_STRING_PRETTY;

	if (json_object_array_length(jarray) > 1 || !(flags & UTIL_JSON_HUMAN))
		fprintf(f_out, "%s\n",
			json_object_to_json_string_ext(jarray, jflag));
	else if (len) {
		struct json_object *jobj;
		jobj = json_object_array_get_idx(jarray, 0);
		fprintf(f_out, "%s\n",
			json_object_to_json_string_ext(jobj, jflag));
	}
	json_object_put(jarray);
}

/* API used to output json object display to specified file */
void __util_display_json_array(FILE * fd, struct json_object *jarray,
			     unsigned long flags)
{
	int len = json_object_array_length(jarray);
	int jflag = JSON_C_TO_STRING_PRETTY;

	if (json_object_array_length(jarray) > 1 || !(flags & UTIL_JSON_HUMAN))
		fprintf(fd, "%s\n",
			json_object_to_json_string_ext(jarray, jflag));
	else if (len) {
		struct json_object *jobj;
		jobj = json_object_array_get_idx(jarray, 0);
		fprintf(fd, "%s\n",
			json_object_to_json_string_ext(jobj, jflag));
	}
	json_object_put(jarray);
}

struct json_object *util_device_to_json(struct accfg_device *device,
					unsigned long flags)
{
	struct json_object *jdevice = json_object_new_object();
	struct json_object *jobj;
	struct accfg_error *error;
	enum accfg_device_state dev_state;
	int int_val;
	unsigned long ulong_val;
	unsigned long long ullong_val;
	bool new_bool;

	if (!jdevice)
		return NULL;

	/* Don't display idle devices */
	if (accfg_device_get_state(device) != ACCFG_DEVICE_ENABLED
			&& !(flags & UTIL_JSON_IDLE)) {
		json_object_put(jdevice);
		return NULL;
	}

	error = (struct accfg_error *)malloc(sizeof(struct accfg_error));
	if (!error) {
		json_object_put(jdevice);
		return NULL;
	}

	jobj = json_object_new_string(accfg_device_get_devname(device));
	if (!jobj)
		goto err;
	json_object_object_add(jdevice, "dev", jobj);

	jobj = json_object_new_int(accfg_device_get_token_limit(device));
	if (!jobj)
		goto err;
	json_object_object_add(jdevice, "token_limit", jobj);

	if (flags & UTIL_JSON_SAVE) {
		free(error);
		return jdevice;
	}

	int_val = accfg_device_get_max_groups(device);
	if (int_val >= 0) {
		jobj = json_object_new_int(int_val);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "max_groups", jobj);
	}

	int_val = accfg_device_get_max_work_queues(device);
	if (int_val >= 0) {
		jobj = json_object_new_int(int_val);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "max_work_queues", jobj);
	}

	int_val = accfg_device_get_max_engines(device);
	if (int_val >= 0) {
		jobj = json_object_new_int(int_val);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "max_engines", jobj);
	}

	int_val = accfg_device_get_max_work_queues_size(device);
	if (int_val >= 0) {
		jobj = json_object_new_int(int_val);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "work_queue_size", jobj);
	}

	jobj = json_object_new_int(accfg_device_get_numa_node(device));
	if (!jobj)
		goto err;
	json_object_object_add(jdevice, "numa_node", jobj);

	if (accfg_device_get_errors(device, error) == 1
			&& (error->val[0] || error->val[1]
				|| error->val[2] || error->val[3])) {
		jobj = json_object_new_array();
		if (!jobj)
			goto err;
		for (int i = 0; i < 4; i++) {
			struct json_object *json_error;

			json_error = util_json_object_hex(error->val[i],
					flags);
			json_object_array_add(jobj, json_error);
		}
		json_object_object_add(jdevice, "errors", jobj);
	}

	ulong_val = accfg_device_get_op_cap(device);
	if (ulong_val > 0) {
		jobj = util_json_object_hex(ulong_val, flags);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "op_cap", jobj);
	}

	jobj = util_json_object_hex(accfg_device_get_gen_cap(device), flags);
	if (!jobj)
		goto err;
	json_object_object_add(jdevice, "gen_cap", jobj);

	jobj = util_json_object_hex(accfg_device_get_version(device),
			flags);
	if (!jobj)
		goto err;
	json_object_object_add(jdevice, "version", jobj);

	dev_state = accfg_device_get_state(device);
	switch (dev_state) {
	case ACCFG_DEVICE_DISABLED:
		jobj = json_object_new_string("disabled");
		break;
	case ACCFG_DEVICE_ENABLED:
		jobj = json_object_new_string("enabled");
		break;
	case ACCFG_DEVICE_UNKNOWN:
	default:
		jobj = json_object_new_string("enabled");
		break;
	}
	if (!jobj)
		goto err;
	json_object_object_add(jdevice, "state", jobj);

	jobj = json_object_new_int(accfg_device_get_max_tokens(device));
	if (!jobj)
		goto err;
	json_object_object_add(jdevice, "max_tokens", jobj);

	ulong_val = accfg_device_get_max_batch_size(device);
	if (ulong_val > 0) {
		jobj = json_object_new_int(ulong_val);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "max_batch_size", jobj);
	}

	int_val = accfg_device_get_ims_size(device);
	if (int_val >= 0) {
		jobj = json_object_new_int(int_val);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "ims_size", jobj);
	}

	int_val = accfg_device_get_max_batch_size(device);
	if (int_val >= 0) {
		jobj = json_object_new_int(int_val);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "max_batch_size", jobj);
	}

	ullong_val = accfg_device_get_max_transfer_size(device);
	if (ullong_val > 0) {
		jobj = json_object_new_int64(ullong_val);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "max_transfer_size", jobj);
	}

	int_val = accfg_device_get_configurable(device);
	if (int_val >= 0) {
		jobj = json_object_new_int(int_val);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "configurable", jobj);
	}

	new_bool = accfg_device_get_pasid_enabled(device);
	if (new_bool) {
		jobj = json_object_new_int(new_bool);
		if (!jobj)
			goto err;
		json_object_object_add(jdevice, "pasid_enabled", jobj);
	}

	jobj = json_object_new_int(accfg_device_get_cdev_major(device));
	if (!jobj)
		goto err;
	json_object_object_add(jdevice, "cdev_major", jobj);

	jobj = json_object_new_int(accfg_device_get_clients(device));
	if (!jobj)
		goto err;
	json_object_object_add(jdevice, "clients", jobj);

	free(error);
	return jdevice;
err:
	free(error);
	json_object_put(jdevice);
	return NULL;
}

struct json_object *util_wq_to_json(struct accfg_wq *wq,
				    unsigned long flags)
{
	struct json_object *jaccfg = json_object_new_object();
	struct json_object *jobj = NULL;
	unsigned long size = ULLONG_MAX;
	enum accfg_wq_mode wq_mode;
	enum accfg_wq_state wq_state;
	const uuid_t *wq_uuid;
	struct json_object *json_uuid;
	char uuid_string[UUID_STR_LEN];
	int int_val;

	if (!jaccfg)
		return NULL;

	wq_state = accfg_wq_get_state(wq);
	/* Don't display idle wqs */
	if (wq_state != ACCFG_WQ_ENABLED && !(flags & UTIL_JSON_IDLE))
		goto err;

	jobj = json_object_new_string(accfg_wq_get_devname(wq));
	if (!jobj)
		goto err;
	json_object_object_add(jaccfg, "dev", jobj);

	wq_mode = accfg_wq_get_mode(wq);
	if (wq_mode == 0)
		jobj = json_object_new_string("shared");
	if (wq_mode == 1)
		jobj = json_object_new_string("dedicated");
	if (!jobj)
		goto err;
	json_object_object_add(jaccfg, "mode", jobj);

	size = accfg_wq_get_size(wq);
	if (size < ULLONG_MAX) {
		jobj = util_json_object_size(size, flags);
		if (jobj)
			json_object_object_add(jaccfg, "size", jobj);
	}

	jobj = json_object_new_int(accfg_wq_get_group_id(wq));
	if (jobj)
		json_object_object_add(jaccfg, "group_id", jobj);

	int_val = accfg_wq_get_priority(wq);
	if (int_val >= 0) {
		jobj = json_object_new_int(int_val);
		if (jobj)
			json_object_object_add(jaccfg, "priority", jobj);
	}

	jobj = json_object_new_int(accfg_wq_get_block_on_fault(wq));
	if (jobj)
		json_object_object_add(jaccfg, "block_on_fault", jobj);

	if (!(flags & UTIL_JSON_SAVE)) {
		jobj = json_object_new_int(accfg_wq_get_cdev_minor(wq));
		if (jobj)
			json_object_object_add(jaccfg, "cdev_minor", jobj);
	}

	jobj = json_object_new_string(wq_type_str[accfg_wq_get_type(wq)]);
	if (jobj)
		json_object_object_add(jaccfg, "type", jobj);

	jobj = json_object_new_string(accfg_wq_get_type_name(wq));
	if (jobj)
		json_object_object_add(jaccfg, "name", jobj);

	jobj = json_object_new_int(accfg_wq_get_threshold(wq));
	if (jobj)
		json_object_object_add(jaccfg, "threshold", jobj);

	if (!(flags & UTIL_JSON_SAVE)) {
		int uuid_found = 0;

		switch (wq_state) {
		case ACCFG_WQ_DISABLED:
			jobj = json_object_new_string("disabled");
			break;
		case ACCFG_WQ_ENABLED:
			jobj = json_object_new_string("enabled");
			break;
		case ACCFG_WQ_QUIESCING:
			jobj = json_object_new_string("quiescing");
			break;
		case ACCFG_WQ_UNKNOWN:
		default:
			jobj = json_object_new_string("unknown");
			break;
		}
		if (jobj)
			json_object_object_add(jaccfg, "state", jobj);

		/* UUID can't be programmed through config file */
		jobj = json_object_new_array();
		accfg_wq_uuid_foreach(wq, wq_uuid) {
			uuid_unparse(*wq_uuid, uuid_string);
			json_uuid = json_object_new_string(uuid_string);
			if (json_uuid)
				json_object_array_add(jobj, json_uuid);
			else
				break;
			uuid_found++;
		}
		if (uuid_found)
			json_object_object_add(jaccfg, "uuid", jobj);
		else
			json_object_put(jobj);

		jobj = json_object_new_int(accfg_wq_get_clients(wq));
		if (jobj)
			json_object_object_add(jaccfg, "clients", jobj);
	}

	return jaccfg;
err:
	json_object_put(jaccfg);
	return NULL;
}

struct json_object *util_engine_to_json(struct accfg_engine *engine,
					unsigned long flags)
{
	struct json_object *jaccfg = json_object_new_object();
	struct json_object *jobj = NULL;

	if (!jaccfg) {
		return NULL;
	}
	jobj = json_object_new_string(accfg_engine_get_devname(engine));
	if (!jobj)
		goto err;
	json_object_object_add(jaccfg, "dev", jobj);

	jobj = json_object_new_int(accfg_engine_get_group_id(engine));
	if (!jobj)
		goto err;
	json_object_object_add(jaccfg, "group_id", jobj);

	return jaccfg;
err:
	json_object_put(jaccfg);
	return NULL;
}