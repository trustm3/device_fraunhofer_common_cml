/*
 * This file is part of trust|me
 * Copyright(c) 2013 - 2021 Fraunhofer AISEC
 * Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 (GPL 2), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Fraunhofer AISEC <trustme@aisec.fraunhofer.de>
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "audit.h"
#include "mem.h"
#include "macro.h"
#include "protobuf.h"
#include "nl.h"

#include <linux/audit.h>
#include <errno.h>
#include <time.h>
#include <string.h>

#include <google/protobuf-c/protobuf-c-text.h>

const char *evcategory[] = { "SUA", "FUA", "SSA", "FSA", "RLE" };
const char *evclass[] = { "GUESTOS_MGMT",	 "TOKEN_MGMT", "CONTAINER_MGMT",
			  "CONTAINER_ISOLATION", "TPM_COMM",   "KAUDIT" };
const char *component[] = { "CMLD", "SCD", "TPM2D" };
const char *result[] = { "SUCCESS", "FAIL" };

static const char *
audit_category_to_string(AUDIT_CATEGORY c)
{
	return evcategory[c];
}

static const char *
audit_evclass_to_string(AUDIT_EVENTCLASS c)
{
	return evclass[c];
}

static const char *
audit_component_to_string(AUDIT_COMPONENT c)
{
	return component[c];
}

AuditRecord *
audit_record_new(AUDIT_CATEGORY category, AUDIT_COMPONENT component, AUDIT_EVENTCLASS evclass,
		 const char *evtype, const char *subject_id, int meta_length,
		 AuditRecord__Meta **metas)
{
	AuditRecord *s = mem_new0(AuditRecord, 1);

	audit_record__init(s);

	char *type = mem_printf("%s.%s.%s.%s", audit_category_to_string(category),
				audit_component_to_string(component),
				audit_evclass_to_string(evclass), evtype);
	s->timestamp = time(NULL);

	if (s->timestamp == (time_t)-1 && EFAULT == errno) {
		ERROR_ERRNO("Failed to get current time");
		return NULL;
	}

	s->type = type;

	if (subject_id)
		s->subject_id = mem_strdup(subject_id);

	s->n_meta = meta_length;
	s->meta = metas;

	return s;
}

int
audit_kernel_send(nl_sock_t *audit_sock, int type, const void *data, size_t len)
{
	int ret;
	nl_msg_t *audit_msg = nl_msg_new();

	IF_NULL_RETVAL(audit_msg, -1);

	nl_msg_set_type(audit_msg, type);
	nl_msg_set_flags(audit_msg, NLM_F_REQUEST | NLM_F_ACK);
	nl_msg_set_buf_unaligned(audit_msg, (char *)data, len);

	ret = nl_msg_send_kernel_verify(audit_sock, audit_msg);

	nl_msg_free(audit_msg);
	return ret;
}

static int
audit_kernel_log_record(const AuditRecord *msg)
{
	int ret = 0;
	char *msg_text = protobuf_c_text_to_string((ProtobufCMessage *)msg, NULL);

	IF_NULL_RETVAL(msg_text, -1);

	DEBUG("kernel log msg='%s'", msg_text);

	/* Open audit netlink socket */
	nl_sock_t *audit_sock;
	if (!(audit_sock = nl_sock_default_new(NETLINK_AUDIT))) {
		ERROR("Failed to allocate audit netlink socket");
		mem_free(msg_text);
		return -1;
	}
	/* send msg to kernel framwork */
	if (-1 ==
	    audit_kernel_send(audit_sock, AUDIT_TRUSTED_APP, msg_text, strlen(msg_text) + 1)) {
		ERROR("Failed to send log record to kernel!");
		ret = -1;
	}
	/* Close netlink connection */
	nl_sock_free(audit_sock);
	mem_free(msg_text);
	return ret;
}

int
audit_kernel_log_event(AUDIT_CATEGORY category, AUDIT_COMPONENT component, AUDIT_EVENTCLASS evclass,
		       const char *evtype, const char *subject_id, int meta_count, ...)
{
	AuditRecord *record = NULL;
	AuditRecord__Meta **metas = NULL;
	int ret = 0;

	if (0 < meta_count) {
		if (0 != (meta_count % 2)) {
			ERROR("Odd number of variadic arguments, aborting...");
			return -1;
		}

		va_list ap;

		va_start(ap, meta_count);

		metas = mem_alloc0((meta_count / 2) * sizeof(AuditRecord__Meta *));
		for (int i = 0; i < meta_count / 2; i++) {
			metas[i] = mem_alloc0(sizeof(AuditRecord__Meta));

			audit_record__meta__init(metas[i]);

			metas[i]->key = mem_strdup(va_arg(ap, const char *));
			metas[i]->value = mem_strdup(va_arg(ap, const char *));
		}

		va_end(ap);
		meta_count /= 2;
	}

	record = audit_record_new(category, component, evclass, evtype, subject_id, meta_count,
				  metas);

	if (!record) {
		ERROR("Failed to create audit record");
		mem_free(metas);
		goto out;
	}

	ret = audit_kernel_log_record(record);

out:
	protobuf_free_message((ProtobufCMessage *)record);

	return ret;
}
