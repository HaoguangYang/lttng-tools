/*
 * filter-grammar-test.c
 *
 * LTTng filter grammar test
 *
 * Copyright 2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#include <common/bytecode/bytecode.hpp>
#include <common/compat/errno.hpp>
#include <common/filter/filter-ast.hpp>
#include <common/filter/filter-parser.hpp>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* For error.h */
int lttng_opt_quiet = 1;
int lttng_opt_verbose;
int lttng_opt_mi;

int main(int argc, char **argv)
{
	struct filter_parser_ctx *ctx;
	int ret;
	int print_xml = 0, generate_ir = 0, generate_bytecode = 0, print_bytecode = 0;
	int argidx;

	for (argidx = 1; argidx < argc; argidx++) {
		if (strcmp(argv[argidx], "-p") == 0)
			print_xml = 1;
		else if (strcmp(argv[argidx], "-i") == 0)
			generate_ir = 1;
		else if (strcmp(argv[argidx], "-b") == 0)
			generate_bytecode = 1;
		else if (strcmp(argv[argidx], "-d") == 0)
			filter_parser_debug = 1;
		else if (strcmp(argv[argidx], "-B") == 0)
			print_bytecode = 1;
	}

	/*
	 * Force generate the bytecode if the user asks to print the bytecode
	 * (can't print it without generating it first).
	 */
	if (print_bytecode) {
		generate_bytecode = 1;
	}

	/*
	 * Force generate the IR if the user asks to generate the bytecode
	 * (the bytecode is generated by visiting the IR).
	 */
	if (generate_bytecode) {
		generate_ir = 1;
	}

	ctx = filter_parser_ctx_alloc(stdin);
	if (!ctx) {
		fprintf(stderr, "Error allocating parser\n");
		goto alloc_error;
	}
	ret = filter_parser_ctx_append_ast(ctx);
	if (ret) {
		fprintf(stderr, "Parse error\n");
		goto parse_error;
	}
	if (print_xml) {
		ret = filter_visitor_print_xml(ctx, stdout, 0);
		if (ret) {
			fflush(stdout);
			fprintf(stderr, "XML print error\n");
			goto parse_error;
		}
	}
	if (generate_ir) {
		printf("Generating IR... ");
		fflush(stdout);
		ret = filter_visitor_ir_generate(ctx);
		if (ret) {
			fprintf(stderr, "Generate IR error\n");
			goto parse_error;
		}
		printf("done\n");

		printf("Validating IR... ");
		fflush(stdout);
		ret = filter_visitor_ir_check_binary_op_nesting(ctx);
		if (ret) {
			goto parse_error;
		}
		printf("done\n");
	}
	if (generate_bytecode) {
		printf("Generating bytecode... ");
		fflush(stdout);
		ret = filter_visitor_bytecode_generate(ctx);
		if (ret) {
			fprintf(stderr, "Generate bytecode error\n");
			goto parse_error;
		}
		printf("done\n");
		printf("Size of bytecode generated: %u bytes.\n",
		       bytecode_get_len(&ctx->bytecode->b));
	}

	if (print_bytecode) {
		unsigned int bytecode_len, len, i;

		len = bytecode_get_len(&ctx->bytecode->b);
		bytecode_len = ctx->bytecode->b.reloc_table_offset;
		printf("Bytecode:\n");
		for (i = 0; i < bytecode_len; i++) {
			printf("0x%X ", ((uint8_t *) ctx->bytecode->b.data)[i]);
		}
		printf("\n");
		printf("Reloc table:\n");
		for (i = bytecode_len; i < len;) {
			printf("{ 0x%X, ", *(uint16_t *) &ctx->bytecode->b.data[i]);
			i += sizeof(uint16_t);
			printf("%s } ", &((char *) ctx->bytecode->b.data)[i]);
			i += strlen(&((char *) ctx->bytecode->b.data)[i]) + 1;
		}
		printf("\n");
	}

	filter_bytecode_free(ctx);
	filter_ir_free(ctx);
	filter_parser_ctx_free(ctx);
	return 0;

parse_error:
	filter_bytecode_free(ctx);
	filter_ir_free(ctx);
	filter_parser_ctx_free(ctx);
alloc_error:
	exit(EXIT_FAILURE);
}
