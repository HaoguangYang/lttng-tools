/*
 * Copyright (C) 2010-2013 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include <common/common.hpp>
#include <common/exception.hpp>
#include <common/time.hpp>
#include <common/uuid.hpp>

#include "ust-app.hpp"
#include "ust-clock.hpp"
#include "ust-registry.hpp"

static
int _lttng_field_statedump(ust_registry_session *session,
		const struct lttng_ust_ctl_field *fields, size_t nr_fields,
		size_t *iter_field, size_t nesting);

static inline
int get_count_order(unsigned int count)
{
	int order;

	order = lttng_fls(count) - 1;
	if (count & (count - 1)) {
		order++;
	}
	LTTNG_ASSERT(order >= 0);
	return order;
}

/*
 * Returns offset where to write in metadata array, or negative error value on error.
 */
static
ssize_t metadata_reserve(ust_registry_session *session, size_t len)
{
	size_t new_len = session->_metadata_len + len;
	size_t new_alloc_len = new_len;
	size_t old_alloc_len = session->_metadata_alloc_len;
	ssize_t ret;

	if (new_alloc_len > (UINT32_MAX >> 1))
		return -EINVAL;
	if ((old_alloc_len << 1) > (UINT32_MAX >> 1))
		return -EINVAL;

	if (new_alloc_len > old_alloc_len) {
		char *newptr;

		new_alloc_len =
			std::max<size_t>(1U << get_count_order(new_alloc_len), old_alloc_len << 1);
		newptr = (char *) realloc(session->_metadata, new_alloc_len);
		if (!newptr)
			return -ENOMEM;
		session->_metadata = newptr;
		/* We zero directly the memory from start of allocation. */
		memset(&session->_metadata[old_alloc_len], 0, new_alloc_len - old_alloc_len);
		session->_metadata_alloc_len = new_alloc_len;
	}
	ret = session->_metadata_len;
	session->_metadata_len += len;
	return ret;
}

static
int metadata_file_append(ust_registry_session *session,
		const char *str, size_t len)
{
	ssize_t written;

	if (session->_metadata_fd < 0) {
		return 0;
	}
	/* Write to metadata file */
	written = lttng_write(session->_metadata_fd, str, len);
	if (written != len) {
		return -1;
	}
	return 0;
}

/*
 * We have exclusive access to our metadata buffer (protected by the
 * ust_lock), so we can do racy operations such as looking for
 * remaining space left in packet and write, since mutual exclusion
 * protects us from concurrent writes.
 */
static ATTR_FORMAT_PRINTF(2, 3)
int lttng_metadata_printf(ust_registry_session *session,
		const char *fmt, ...)
{
	char *str = NULL;
	size_t len;
	va_list ap;
	ssize_t offset;
	int ret;

	va_start(ap, fmt);
	ret = vasprintf(&str, fmt, ap);
	va_end(ap);
	if (ret < 0)
		return -ENOMEM;

	len = strlen(str);
	offset = metadata_reserve(session, len);
	if (offset < 0) {
		ret = offset;
		goto end;
	}
	memcpy(&session->_metadata[offset], str, len);
	ret = metadata_file_append(session, str, len);
	if (ret) {
		PERROR("Error appending to metadata file");
		goto end;
	}
	DBG3("Append to metadata: \"%s\"", str);
	ret = 0;

end:
	free(str);
	return ret;
}

static
int print_tabs(ust_registry_session *session, size_t nesting)
{
	size_t i;

	for (i = 0; i < nesting; i++) {
		int ret;

		ret = lttng_metadata_printf(session, "	");
		if (ret) {
			return ret;
		}
	}
	return 0;
}

static
void sanitize_ctf_identifier(char *out, const char *in)
{
	size_t i;

	for (i = 0; i < LTTNG_UST_ABI_SYM_NAME_LEN; i++) {
		switch (in[i]) {
		case '.':
		case '$':
		case ':':
			out[i] = '_';
			break;
		default:
			out[i] = in[i];
		}
	}
}

static
int print_escaped_ctf_string(ust_registry_session *session, const char *string)
{
	int ret = 0;
	size_t i;
	char cur;

	i = 0;
	cur = string[i];
	while (cur != '\0') {
		switch (cur) {
		case '\n':
			ret = lttng_metadata_printf(session, "%s", "\\n");
			break;
		case '\\':
		case '"':
			ret = lttng_metadata_printf(session, "%c", '\\');
			if (ret) {
				goto error;
			}
			/* We still print the current char */
			/* Fallthrough */
		default:
			ret = lttng_metadata_printf(session, "%c", cur);
			break;
		}

		if (ret) {
			goto error;
		}

		cur = string[++i];
	}
error:
	return ret;
}

/* Called with session registry mutex held. */
static
int ust_metadata_enum_statedump(ust_registry_session *session,
		const char *enum_name,
		uint64_t enum_id,
		const struct lttng_ust_ctl_integer_type *container_type,
		const char *field_name, size_t *iter_field, size_t nesting)
{
	struct ust_registry_enum *reg_enum;
	const struct lttng_ust_ctl_enum_entry *entries;
	size_t nr_entries;
	int ret = 0;
	size_t i;
	char identifier[LTTNG_UST_ABI_SYM_NAME_LEN];

	rcu_read_lock();
	reg_enum = ust_registry_lookup_enum_by_id(session, enum_name, enum_id);
	rcu_read_unlock();
	/* reg_enum can still be used because session registry mutex is held. */
	if (!reg_enum) {
		ret = -ENOENT;
		goto end;
	}
	entries = reg_enum->entries;
	nr_entries = reg_enum->nr_entries;

	ret = print_tabs(session, nesting);
	if (ret) {
		goto end;
	}
	ret = lttng_metadata_printf(session,
		"enum : integer { size = %u; align = %u; signed = %u; encoding = %s; base = %u; } {\n",
		container_type->size,
		container_type->alignment,
		container_type->signedness,
		(container_type->encoding == lttng_ust_ctl_encode_none)
			? "none"
			: (container_type->encoding == lttng_ust_ctl_encode_UTF8)
				? "UTF8"
				: "ASCII",
		container_type->base);
	if (ret) {
	        goto end;
	}
	nesting++;
	/* Dump all entries */
	for (i = 0; i < nr_entries; i++) {
		const struct lttng_ust_ctl_enum_entry *entry = &entries[i];
		int j, len;

		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		ret = lttng_metadata_printf(session,
				"\"");
		if (ret) {
			goto end;
		}
		len = strlen(entry->string);
		/* Escape the character '"' */
		for (j = 0; j < len; j++) {
			char c = entry->string[j];

			switch (c) {
			case '"':
				ret = lttng_metadata_printf(session,
						"\\\"");
				break;
			case '\\':
				ret = lttng_metadata_printf(session,
						"\\\\");
				break;
			default:
				ret = lttng_metadata_printf(session,
						"%c", c);
				break;
			}
			if (ret) {
				goto end;
			}
		}
		ret = lttng_metadata_printf(session, "\"");
		if (ret) {
			goto end;
		}

		if (entry->u.extra.options &
				LTTNG_UST_CTL_UST_ENUM_ENTRY_OPTION_IS_AUTO) {
			ret = lttng_metadata_printf(session, ",\n");
			if (ret) {
				goto end;
			}
		} else {
			ret = lttng_metadata_printf(session,
					" = ");
			if (ret) {
				goto end;
			}

			if (entry->start.signedness) {
				ret = lttng_metadata_printf(session,
					"%" PRId64, entry->start.value);
			} else {
				ret = lttng_metadata_printf(session,
					"%" PRIu64, entry->start.value);
			}
			if (ret) {
				goto end;
			}

			if (entry->start.signedness == entry->end.signedness &&
					entry->start.value ==
						entry->end.value) {
				ret = lttng_metadata_printf(session, ",\n");
			} else {
				if (entry->end.signedness) {
					ret = lttng_metadata_printf(session,
						" ... %" PRId64 ",\n",
						entry->end.value);
				} else {
					ret = lttng_metadata_printf(session,
						" ... %" PRIu64 ",\n",
						entry->end.value);
				}
			}
			if (ret) {
				goto end;
			}
		}
	}
	nesting--;
	sanitize_ctf_identifier(identifier, field_name);
	ret = print_tabs(session, nesting);
	if (ret) {
		goto end;
	}
	ret = lttng_metadata_printf(session, "} _%s;\n",
			identifier);
end:
	(*iter_field)++;
	return ret;
}

static
int _lttng_variant_statedump(ust_registry_session *session,
		uint32_t nr_choices, const char *tag_name,
		uint32_t alignment,
		const struct lttng_ust_ctl_field *fields, size_t nr_fields,
		size_t *iter_field, size_t nesting)
{
	const struct lttng_ust_ctl_field *variant = &fields[*iter_field];
	uint32_t i;
	int ret;
	char identifier[LTTNG_UST_ABI_SYM_NAME_LEN];

	(*iter_field)++;
	sanitize_ctf_identifier(identifier, tag_name);
	if (alignment) {
		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		ret = lttng_metadata_printf(session,
		"struct { } align(%u) _%s_padding;\n",
				alignment * CHAR_BIT,
				variant->name);
		if (ret) {
			goto end;
		}
	}
	ret = print_tabs(session, nesting);
	if (ret) {
		goto end;
	}
	ret = lttng_metadata_printf(session,
			"variant <_%s> {\n",
			identifier);
	if (ret) {
		goto end;
	}

	for (i = 0; i < nr_choices; i++) {
		if (*iter_field >= nr_fields) {
			ret = -EOVERFLOW;
			goto end;
		}
		ret = _lttng_field_statedump(session,
				fields, nr_fields,
				iter_field, nesting + 1);
		if (ret) {
			goto end;
		}
	}
	sanitize_ctf_identifier(identifier, variant->name);
	ret = print_tabs(session, nesting);
	if (ret) {
		goto end;
	}
	ret = lttng_metadata_printf(session,
			"} _%s;\n",
			identifier);
	if (ret) {
		goto end;
	}
end:
	return ret;
}

static
int _lttng_field_statedump(ust_registry_session *session,
		const struct lttng_ust_ctl_field *fields, size_t nr_fields,
		size_t *iter_field, size_t nesting)
{
	int ret = 0;
	const char *bo_be = " byte_order = be;";
	const char *bo_le = " byte_order = le;";
	const char *bo_native = "";
	const char *bo_reverse;
	const struct lttng_ust_ctl_field *field;

	if (*iter_field >= nr_fields) {
		ret = -EOVERFLOW;
		goto end;
	}
	field = &fields[*iter_field];

	if (session->_byte_order == BIG_ENDIAN) {
		bo_reverse = bo_le;
	} else {
		bo_reverse = bo_be;
	}

	switch (field->type.atype) {
	case lttng_ust_ctl_atype_integer:
		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		ret = lttng_metadata_printf(session,
			"integer { size = %u; align = %u; signed = %u; encoding = %s; base = %u;%s } _%s;\n",
			field->type.u.integer.size,
			field->type.u.integer.alignment,
			field->type.u.integer.signedness,
			(field->type.u.integer.encoding == lttng_ust_ctl_encode_none)
				? "none"
				: (field->type.u.integer.encoding == lttng_ust_ctl_encode_UTF8)
					? "UTF8"
					: "ASCII",
			field->type.u.integer.base,
			field->type.u.integer.reverse_byte_order ? bo_reverse : bo_native,
			field->name);
		(*iter_field)++;
		break;
	case lttng_ust_ctl_atype_enum:
		ret = ust_metadata_enum_statedump(session,
			field->type.u.legacy.basic.enumeration.name,
			field->type.u.legacy.basic.enumeration.id,
			&field->type.u.legacy.basic.enumeration.container_type,
			field->name, iter_field, nesting);
		break;
	case lttng_ust_ctl_atype_float:
		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		ret = lttng_metadata_printf(session,
			"floating_point { exp_dig = %u; mant_dig = %u; align = %u;%s } _%s;\n",
			field->type.u._float.exp_dig,
			field->type.u._float.mant_dig,
			field->type.u._float.alignment,
			field->type.u._float.reverse_byte_order ? bo_reverse : bo_native,
			field->name);
		(*iter_field)++;
		break;
	case lttng_ust_ctl_atype_array:
	{
		const struct lttng_ust_ctl_basic_type *elem_type;

		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		elem_type = &field->type.u.legacy.array.elem_type;
		/* Only integers are currently supported in arrays. */
		if (elem_type->atype != lttng_ust_ctl_atype_integer) {
			ret = -EINVAL;
			goto end;
		}
		ret = lttng_metadata_printf(session,
			"integer { size = %u; align = %u; signed = %u; encoding = %s; base = %u;%s } _%s[%u];\n",
			elem_type->u.basic.integer.size,
			elem_type->u.basic.integer.alignment,
			elem_type->u.basic.integer.signedness,
			(elem_type->u.basic.integer.encoding == lttng_ust_ctl_encode_none)
				? "none"
				: (elem_type->u.basic.integer.encoding == lttng_ust_ctl_encode_UTF8)
					? "UTF8"
					: "ASCII",
			elem_type->u.basic.integer.base,
			elem_type->u.basic.integer.reverse_byte_order ? bo_reverse : bo_native,
			field->name, field->type.u.legacy.array.length);
		(*iter_field)++;
		break;
	}
	case lttng_ust_ctl_atype_array_nestable:
	{
		uint32_t array_length;
		const struct lttng_ust_ctl_field *array_nestable;
		const struct lttng_ust_ctl_type *elem_type;

		array_length = field->type.u.array_nestable.length;
		(*iter_field)++;

		if (*iter_field >= nr_fields) {
			ret = -EOVERFLOW;
			goto end;
		}
		array_nestable = &fields[*iter_field];
		elem_type = &array_nestable->type;

		/* Only integers are currently supported in arrays. */
		if (elem_type->atype != lttng_ust_ctl_atype_integer) {
			ret = -EINVAL;
			goto end;
		}

		if (field->type.u.array_nestable.alignment) {
			ret = print_tabs(session, nesting);
			if (ret) {
				goto end;
			}
			ret = lttng_metadata_printf(session,
				"struct { } align(%u) _%s_padding;\n",
				field->type.u.array_nestable.alignment * CHAR_BIT,
				field->name);
			if (ret) {
				goto end;
			}
		}

		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		ret = lttng_metadata_printf(session,
			"integer { size = %u; align = %u; signed = %u; encoding = %s; base = %u;%s } _%s[%u];\n",
			elem_type->u.integer.size,
			elem_type->u.integer.alignment,
			elem_type->u.integer.signedness,
			(elem_type->u.integer.encoding == lttng_ust_ctl_encode_none)
				? "none"
				: (elem_type->u.integer.encoding == lttng_ust_ctl_encode_UTF8)
					? "UTF8"
					: "ASCII",
			elem_type->u.integer.base,
			elem_type->u.integer.reverse_byte_order ? bo_reverse : bo_native,
			field->name, array_length);
		(*iter_field)++;
		break;
	}
	case lttng_ust_ctl_atype_sequence:
	{
		const struct lttng_ust_ctl_basic_type *elem_type;
		const struct lttng_ust_ctl_basic_type *length_type;

		elem_type = &field->type.u.legacy.sequence.elem_type;
		length_type = &field->type.u.legacy.sequence.length_type;
		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}

		/* Only integers are currently supported in sequences. */
		if (elem_type->atype != lttng_ust_ctl_atype_integer) {
			ret = -EINVAL;
			goto end;
		}

		ret = lttng_metadata_printf(session,
			"integer { size = %u; align = %u; signed = %u; encoding = %s; base = %u;%s } __%s_length;\n",
			length_type->u.basic.integer.size,
			(unsigned int) length_type->u.basic.integer.alignment,
			length_type->u.basic.integer.signedness,
			(length_type->u.basic.integer.encoding == lttng_ust_ctl_encode_none)
				? "none"
				: ((length_type->u.basic.integer.encoding == lttng_ust_ctl_encode_UTF8)
					? "UTF8"
					: "ASCII"),
			length_type->u.basic.integer.base,
			length_type->u.basic.integer.reverse_byte_order ? bo_reverse : bo_native,
			field->name);
		if (ret) {
			goto end;
		}

		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		ret = lttng_metadata_printf(session,
			"integer { size = %u; align = %u; signed = %u; encoding = %s; base = %u;%s } _%s[ __%s_length ];\n",
			elem_type->u.basic.integer.size,
			(unsigned int) elem_type->u.basic.integer.alignment,
			elem_type->u.basic.integer.signedness,
			(elem_type->u.basic.integer.encoding == lttng_ust_ctl_encode_none)
				? "none"
				: ((elem_type->u.basic.integer.encoding == lttng_ust_ctl_encode_UTF8)
					? "UTF8"
					: "ASCII"),
			elem_type->u.basic.integer.base,
			elem_type->u.basic.integer.reverse_byte_order ? bo_reverse : bo_native,
			field->name,
			field->name);
		(*iter_field)++;
		break;
	}
	case lttng_ust_ctl_atype_sequence_nestable:
	{
		const struct lttng_ust_ctl_field *sequence_nestable;
		const struct lttng_ust_ctl_type *elem_type;

		(*iter_field)++;
		if (*iter_field >= nr_fields) {
			ret = -EOVERFLOW;
			goto end;
		}
		sequence_nestable = &fields[*iter_field];
		elem_type = &sequence_nestable->type;

		/* Only integers are currently supported in sequences. */
		if (elem_type->atype != lttng_ust_ctl_atype_integer) {
			ret = -EINVAL;
			goto end;
		}

		if (field->type.u.sequence_nestable.alignment) {
			ret = print_tabs(session, nesting);
			if (ret) {
				goto end;
			}
			ret = lttng_metadata_printf(session,
				"struct { } align(%u) _%s_padding;\n",
				field->type.u.sequence_nestable.alignment * CHAR_BIT,
				field->name);
			if (ret) {
				goto end;
			}
		}

		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		ret = lttng_metadata_printf(session,
			"integer { size = %u; align = %u; signed = %u; encoding = %s; base = %u;%s } _%s[ _%s ];\n",
			elem_type->u.integer.size,
			(unsigned int) elem_type->u.integer.alignment,
			elem_type->u.integer.signedness,
			(elem_type->u.integer.encoding == lttng_ust_ctl_encode_none)
				? "none"
				: ((elem_type->u.integer.encoding == lttng_ust_ctl_encode_UTF8)
					? "UTF8"
					: "ASCII"),
			elem_type->u.integer.base,
			elem_type->u.integer.reverse_byte_order ? bo_reverse : bo_native,
			field->name,
			field->type.u.sequence_nestable.length_name);
		(*iter_field)++;
		break;
	}
	case lttng_ust_ctl_atype_string:
		/* Default encoding is UTF8 */
		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		ret = lttng_metadata_printf(session,
			"string%s _%s;\n",
			field->type.u.string.encoding == lttng_ust_ctl_encode_ASCII ?
				" { encoding = ASCII; }" : "",
			field->name);
		(*iter_field)++;
		break;
	case lttng_ust_ctl_atype_variant:
		ret = _lttng_variant_statedump(session,
				field->type.u.legacy.variant.nr_choices,
				field->type.u.legacy.variant.tag_name,
				0,
				fields, nr_fields, iter_field, nesting);
		if (ret) {
			goto end;
		}
		break;
	case lttng_ust_ctl_atype_variant_nestable:
		ret = _lttng_variant_statedump(session,
				field->type.u.variant_nestable.nr_choices,
				field->type.u.variant_nestable.tag_name,
				field->type.u.variant_nestable.alignment,
				fields, nr_fields, iter_field, nesting);
		if (ret) {
			goto end;
		}
		break;
	case lttng_ust_ctl_atype_struct:
		if (field->type.u.legacy._struct.nr_fields != 0) {
			/* Currently only 0-length structures are supported. */
			ret = -EINVAL;
			goto end;
		}
		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		ret = lttng_metadata_printf(session,
			"struct {} _%s;\n",
			field->name);
		(*iter_field)++;
		break;
	case lttng_ust_ctl_atype_struct_nestable:
		if (field->type.u.struct_nestable.nr_fields != 0) {
			/* Currently only 0-length structures are supported. */
			ret = -EINVAL;
			goto end;
		}
		ret = print_tabs(session, nesting);
		if (ret) {
			goto end;
		}
		if (field->type.u.struct_nestable.alignment) {
			ret = lttng_metadata_printf(session,
				"struct {} align(%u) _%s;\n",
				field->type.u.struct_nestable.alignment * CHAR_BIT,
				field->name);
			if (ret) {
				goto end;
			}
		} else {
			ret = lttng_metadata_printf(session,
				"struct {} _%s;\n",
				field->name);
		}
		(*iter_field)++;
		break;
	case lttng_ust_ctl_atype_enum_nestable:
	{
		const struct lttng_ust_ctl_field *container_field;
		const struct lttng_ust_ctl_type *container_type;

		(*iter_field)++;
		if (*iter_field >= nr_fields) {
			ret = -EOVERFLOW;
			goto end;
		}
		container_field = &fields[*iter_field];
		container_type = &container_field->type;

		/* Only integers are supported as container types. */
		if (container_type->atype != lttng_ust_ctl_atype_integer) {
			ret = -EINVAL;
			goto end;
		}
		ret = ust_metadata_enum_statedump(session,
			field->type.u.enum_nestable.name,
			field->type.u.enum_nestable.id,
			&container_type->u.integer,
			field->name, iter_field, nesting);
		break;
	}
	default:
		ret = -EINVAL;
	}
end:
	return ret;
}

static
int _lttng_context_metadata_statedump(ust_registry_session *session,
		size_t nr_ctx_fields,
		struct lttng_ust_ctl_field *ctx)
{
	int ret = 0;
	size_t i = 0;

	if (!ctx)
		return 0;
	for (;;) {
		if (i >= nr_ctx_fields) {
			break;
		}
		ret = _lttng_field_statedump(session, ctx,
				nr_ctx_fields, &i, 2);
		if (ret) {
			break;
		}
	}
	return ret;
}

static
int _lttng_fields_metadata_statedump(ust_registry_session *session,
		struct ust_registry_event *event)
{
	int ret = 0;
	size_t i = 0;

	for (;;) {
		if (i >= event->nr_fields) {
			break;
		}
		ret = _lttng_field_statedump(session, event->fields,
				event->nr_fields, &i, 2);
		if (ret) {
			break;
		}
	}
	return ret;
}

/*
 * Should be called with session registry mutex held.
 */
int ust_metadata_event_statedump(ust_registry_session *session,
		struct ust_registry_channel *chan,
		struct ust_registry_event *event)
{
	int ret = 0;

	/* Don't dump metadata events */
	if (chan->chan_id == -1U)
		return 0;

	/*
	 * We don't want to output an event's metadata before its parent
	 * stream's metadata.  If the stream's metadata hasn't been output yet,
	 * skip this event.  Its metadata will be output when we output the
	 * stream's metadata.
	 */
	if (!chan->metadata_dumped || event->metadata_dumped) {
		return 0;
	}

	ret = lttng_metadata_printf(session,
		"event {\n"
		"	name = \"%s\";\n"
		"	id = %u;\n"
		"	stream_id = %u;\n",
		event->name,
		event->id,
		chan->chan_id);
	if (ret) {
		goto end;
	}

	ret = lttng_metadata_printf(session,
		"	loglevel = %d;\n",
		event->loglevel_value);
	if (ret) {
		goto end;
	}

	if (event->model_emf_uri) {
		ret = lttng_metadata_printf(session,
			"	model.emf.uri = \"%s\";\n",
			event->model_emf_uri);
		if (ret) {
			goto end;
		}
	}

	ret = lttng_metadata_printf(session,
		"	fields := struct {\n"
		);
	if (ret) {
		goto end;
	}

	ret = _lttng_fields_metadata_statedump(session, event);
	if (ret) {
		goto end;
	}

	ret = lttng_metadata_printf(session,
		"	};\n"
		"};\n\n");
	if (ret) {
		goto end;
	}
	event->metadata_dumped = 1;

end:
	return ret;
}

/*
 * Should be called with session registry mutex held.
 *
 * RCU read lock must be held by the caller.
 */
int ust_metadata_channel_statedump(ust_registry_session *session,
		struct ust_registry_channel *chan)
{
	int ret;

	ASSERT_RCU_READ_LOCKED();

	/* Don't dump metadata events */
	if (chan->chan_id == -1U)
		return 0;

	if (!chan->header_type)
		return -EINVAL;

	ret = lttng_metadata_printf(session,
		"stream {\n"
		"	id = %u;\n"
		"	event.header := %s;\n"
		"	packet.context := struct packet_context;\n",
		chan->chan_id,
		chan->header_type == LTTNG_UST_CTL_CHANNEL_HEADER_COMPACT ?
			"struct event_header_compact" :
			"struct event_header_large");
	if (ret) {
		return ret;
	}

	if (chan->ctx_fields) {
		ret = lttng_metadata_printf(session,
			"	event.context := struct {\n");
		if (ret) {
			return ret;
		}
	}
	ret = _lttng_context_metadata_statedump(session,
		chan->nr_ctx_fields,
		chan->ctx_fields);
	if (ret) {
		return ret;
	}
	if (chan->ctx_fields) {
		ret = lttng_metadata_printf(session,
			"	};\n");
		if (ret) {
			return ret;
		}
	}

	ret = lttng_metadata_printf(session,
		"};\n\n");
	if (ret) {
		return ret;
	}

	/* Flag success of metadata dump. */
	chan->metadata_dumped = 1;

	/*
	 * Output the metadata of any existing event.
	 *
	 * Sort the events by id.  This is not necessary, but it's nice to have
	 * a more predictable order in the metadata file.
	 */
	std::vector<ust_registry_event *> events;
	{
		cds_lfht_iter event_iter;
		ust_registry_event *event;
		cds_lfht_for_each_entry(chan->events->ht, &event_iter, event,
				node.node) {
			events.push_back(event);
		}
	}

	std::sort(events.begin(), events.end(),
		[] (ust_registry_event *a, ust_registry_event *b) {
			return a->id < b->id;
		});

	for (ust_registry_event *event : events) {
		ust_metadata_event_statedump(session, chan, event);
	}

	return 0;
}

static
int _lttng_stream_packet_context_declare(ust_registry_session *session)
{
	return lttng_metadata_printf(session,
		"struct packet_context {\n"
		"	uint64_clock_monotonic_t timestamp_begin;\n"
		"	uint64_clock_monotonic_t timestamp_end;\n"
		"	uint64_t content_size;\n"
		"	uint64_t packet_size;\n"
		"	uint64_t packet_seq_num;\n"
		"	unsigned long events_discarded;\n"
		"	uint32_t cpu_id;\n"
		"};\n\n"
		);
}

/*
 * Compact header:
 * id: range: 0 - 30.
 * id 31 is reserved to indicate an extended header.
 *
 * Large header:
 * id: range: 0 - 65534.
 * id 65535 is reserved to indicate an extended header.
 */
static
int _lttng_event_header_declare(ust_registry_session *session)
{
	return lttng_metadata_printf(session,
	"struct event_header_compact {\n"
	"	enum : uint5_t { compact = 0 ... 30, extended = 31 } id;\n"
	"	variant <id> {\n"
	"		struct {\n"
	"			uint27_clock_monotonic_t timestamp;\n"
	"		} compact;\n"
	"		struct {\n"
	"			uint32_t id;\n"
	"			uint64_clock_monotonic_t timestamp;\n"
	"		} extended;\n"
	"	} v;\n"
	"} align(%u);\n"
	"\n"
	"struct event_header_large {\n"
	"	enum : uint16_t { compact = 0 ... 65534, extended = 65535 } id;\n"
	"	variant <id> {\n"
	"		struct {\n"
	"			uint32_clock_monotonic_t timestamp;\n"
	"		} compact;\n"
	"		struct {\n"
	"			uint32_t id;\n"
	"			uint64_clock_monotonic_t timestamp;\n"
	"		} extended;\n"
	"	} v;\n"
	"} align(%u);\n\n",
	session->_uint32_t_alignment,
	session->_uint16_t_alignment
	);
}

static
int print_metadata_session_information(ust_registry_session *registry)
{
	int ret;
	struct ltt_session *session = NULL;
	char creation_datetime[ISO8601_STR_LEN];

	rcu_read_lock();
	session = session_find_by_id(registry->_tracing_id);
	if (!session) {
		ret = -1;
		goto error;
	}

	/* Print the trace name */
	ret = lttng_metadata_printf(registry, "	trace_name = \"");
	if (ret) {
		goto error;
	}

	/*
	 * This is necessary since the creation time is present in the session
	 * name when it is generated.
	 */
	if (session->has_auto_generated_name) {
		ret = print_escaped_ctf_string(registry, DEFAULT_SESSION_NAME);
	} else {
		ret = print_escaped_ctf_string(registry, session->name);
	}
	if (ret) {
		goto error;
	}

	ret = lttng_metadata_printf(registry, "\";\n");
	if (ret) {
		goto error;
	}

	/* Prepare creation time */
	ret = time_to_iso8601_str(session->creation_time, creation_datetime,
			sizeof(creation_datetime));
	if (ret) {
		goto error;
	}

	/* Output the reste of the information */
	ret = lttng_metadata_printf(registry,
			"	trace_creation_datetime = \"%s\";\n"
			"	hostname = \"%s\";\n",
			creation_datetime, session->hostname);
	if (ret) {
		goto error;
	}

error:
	if (session) {
		session_put(session);
	}
	rcu_read_unlock();
	return ret;
}

static
int print_metadata_app_information(ust_registry_session *registry)
{
	if (registry->get_buffering_scheme() != LTTNG_BUFFER_PER_PID) {
		return 0;
	}

	const auto *per_pid_session = static_cast<const ust_registry_session_per_pid *>(registry);

	char datetime[ISO8601_STR_LEN];
	int ret = time_to_iso8601_str(
		per_pid_session->_app_creation_time, datetime, sizeof(datetime));
	if (ret) {
		return ret;
	}

	ret = lttng_metadata_printf(registry,
		"	tracer_patchlevel = %u;\n"
		"	vpid = %d;\n"
		"	procname = \"%s\";\n"
		"	vpid_datetime = \"%s\";\n",
		per_pid_session->_tracer_patch_level_version, (int) per_pid_session->_vpid,
		per_pid_session->_procname.c_str(), datetime);
	return ret;
}

/*
 * Should be called with session registry mutex held.
 */
int ust_metadata_session_statedump(ust_registry_session *session)
{
	int ret = 0;
	char trace_uuid_str[LTTNG_UUID_STR_LEN];

	LTTNG_ASSERT(session);

	lttng_uuid_to_str(session->_uuid, trace_uuid_str);

	/* For crash ABI */
	ret = lttng_metadata_printf(session,
		"/* CTF %u.%u */\n\n",
		CTF_SPEC_MAJOR,
		CTF_SPEC_MINOR);
	if (ret) {
		goto end;
	}

	ret = lttng_metadata_printf(session,
		"typealias integer { size = 8; align = %u; signed = false; } := uint8_t;\n"
		"typealias integer { size = 16; align = %u; signed = false; } := uint16_t;\n"
		"typealias integer { size = 32; align = %u; signed = false; } := uint32_t;\n"
		"typealias integer { size = 64; align = %u; signed = false; } := uint64_t;\n"
		"typealias integer { size = %u; align = %u; signed = false; } := unsigned long;\n"
		"typealias integer { size = 5; align = 1; signed = false; } := uint5_t;\n"
		"typealias integer { size = 27; align = 1; signed = false; } := uint27_t;\n"
		"\n"
		"trace {\n"
		"	major = %u;\n"
		"	minor = %u;\n"
		"	uuid = \"%s\";\n"
		"	byte_order = %s;\n"
		"	packet.header := struct {\n"
		"		uint32_t magic;\n"
		"		uint8_t  uuid[16];\n"
		"		uint32_t stream_id;\n"
		"		uint64_t stream_instance_id;\n"
		"	};\n"
		"};\n\n",
		session->_uint8_t_alignment,
		session->_uint16_t_alignment,
		session->_uint32_t_alignment,
		session->_uint64_t_alignment,
		session->_bits_per_long,
		session->_long_alignment,
		CTF_SPEC_MAJOR,
		CTF_SPEC_MINOR,
		trace_uuid_str,
		session->_byte_order == BIG_ENDIAN ? "be" : "le"
		);
	if (ret) {
		goto end;
	}

	ret = lttng_metadata_printf(session,
		"env {\n"
		"	domain = \"ust\";\n"
		"	tracer_name = \"lttng-ust\";\n"
		"	tracer_major = %u;\n"
		"	tracer_minor = %u;\n"
		"	tracer_buffering_scheme = \"%s\";\n"
		"	tracer_buffering_id = %u;\n"
		"	architecture_bit_width = %u;\n",
		session->_app_tracer_version_major, session->_app_tracer_version_minor,
		session->get_buffering_scheme() == LTTNG_BUFFER_PER_PID ? "pid" : "uid",
		session->get_buffering_scheme() == LTTNG_BUFFER_PER_PID ?
			(int) static_cast<ust_registry_session_per_pid *>(session)->_vpid :
			(int) static_cast<ust_registry_session_per_uid *>(session)->_tracing_uid,
		session->_bits_per_long);
	if (ret) {
		goto end;
	}

	ret = print_metadata_session_information(session);
	if (ret) {
		goto end;
	}

	/*
	 * If per-application registry, we can output extra information
	 * about the application.
	 */
	ret = print_metadata_app_information(session);
	if (ret) {
		goto end;
	}

	ret = lttng_metadata_printf(session,
		"};\n\n"
		);
	if (ret) {
		goto end;
	}

	try {
		const lttng::ust::clock_attributes_sample clock;

		ret = lttng_metadata_printf(session,
				"clock {\n"
				"	name = \"%s\";\n",
				clock._name.c_str());
		if (ret) {
			goto end;
		}

		if (clock._uuid) {
			char clock_uuid_str[LTTNG_UUID_STR_LEN];

			lttng_uuid_to_str(*clock._uuid, clock_uuid_str);
			ret = lttng_metadata_printf(
				session, "	uuid = \"%s\";\n", clock_uuid_str);
			if (ret) {
				goto end;
			}
		}

		ret = lttng_metadata_printf(session,
				"	description = \"%s\";\n"
				"	freq = %" PRIu64 "; /* Frequency, in Hz */\n"
				"	/* clock value offset from Epoch is: offset * (1/freq) */\n"
				"	offset = %" PRId64 ";\n"
				"};\n\n",
				clock._description.c_str(), clock._frequency, clock._offset);
		if (ret) {
			goto end;
		}

		ret = lttng_metadata_printf(session,
				"typealias integer {\n"
				"	size = 27; align = 1; signed = false;\n"
				"	map = clock.%s.value;\n"
				"} := uint27_clock_monotonic_t;\n"
				"\n"
				"typealias integer {\n"
				"	size = 32; align = %u; signed = false;\n"
				"	map = clock.%s.value;\n"
				"} := uint32_clock_monotonic_t;\n"
				"\n"
				"typealias integer {\n"
				"	size = 64; align = %u; signed = false;\n"
				"	map = clock.%s.value;\n"
				"} := uint64_clock_monotonic_t;\n\n",
				clock._name.c_str(), session->_uint32_t_alignment,
				clock._name.c_str(), session->_uint64_t_alignment,
				clock._name.c_str());
		if (ret) {
			goto end;
		}
	} catch (const std::exception &ex) {
		ERR("Failed to serialize clock description: %s", ex.what());
		ret = -1;
		goto end;
	}

	ret = _lttng_stream_packet_context_declare(session);
	if (ret) {
		goto end;
	}

	ret = _lttng_event_header_declare(session);
	if (ret) {
		goto end;
	}

end:
	return ret;
}
