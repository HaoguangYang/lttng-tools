#!/bin/bash
#
# Copyright (C) 2017 Julien Desfossez <jdesfossez@efficios.com>
#
# SPDX-License-Identifier: LGPL-2.1-only

# Clean everything under directory but keep directory
function clean_path ()
{
	local path=$1
	# Use -u from bash top prevent empty expansion of variable yielding a
	# list of current directory from find.
	set -u
	find $path -mindepth 1 -maxdepth 1 -exec rm -rf '{}' \;
	set +u
}

# The extglob shell option must be enabled to use the pattern generated by this
# function (shopt -s extglob/ shopt -u extglob).
# The pattern returned by this function is to validate the form:
#    YYYYMMDDTHHMMSS[+-]HHMM-YYYYMMDDTHHMMSS[+-]HHMM
#
# Where YYYYMMDD is today or tomorrow. Tomorrow must be supported in case where
# the chunks are generated close to midnight and one ends up the following day.
function get_chunk_pattern ()
{
	local today=$1
	tommorow=$(date +%Y%m%d -d "${today}+1days")
	pattern_hour_min="[0-9][0-9][0-9][0-9]"
	pattern_hour_min_sec="${pattern_hour_min}[0-9][0-9]"

	base_pattern="@(${today}|${tommorow})T${pattern_hour_min_sec}[+-]${pattern_hour_min}"

	# Return the pattern
	# YYYYMMDDTHHMMSS[+-]HHMM-YYYYMMDDTHHMMSS[+-]HHMM
	echo "${base_pattern}-${base_pattern}"
}

function validate_test_chunks ()
{
	local_path=$1
	today=$2
	app_path=$3
	shift 3
	domains=("$@")

	local path=
	local chunk_pattern=$(get_chunk_pattern ${today})

	# Enable extglob for the use of chunk_pattern
	shopt -s extglob

	# Validate that only 2 chunks are present
	nb_chunk=$(ls -A $local_path | wc -l)
	test $nb_chunk -eq 2
	ok $? "${local_path} contains 2 chunks only"

	# Check if the first and second chunk folders exist and they contain a ${app_path}/metadata file.
	for chunk in $(seq 0 1); do
		path=$(ls $local_path/${chunk_pattern}-${chunk}/${app_path}/metadata)
		ok $? "Chunk ${chunk} exists based on path $path"
	done

	# Make sure we don't have anything else in the first 2 chunk directories
	# besides the kernel folder.
	for chunk in $(seq 0 1); do
		local stale_files

		stale_files=$(ls -A $local_path/${chunk_pattern}-${chunk})
		for domain in "${domains[@]}"; do
			stale_files=$(echo "$stale_files" | grep -v $domain)
		done
		nr_stale=$(echo -n "$stale_files" | wc -l)
		ok "$nr_stale" "No stale folders in chunk ${chunk} directory"
	done

	# We expect a complete session of 30 events
	validate_trace_count $EVENT_NAME $local_path 30

	# Chunk 1: 10 events
	validate_trace_count $EVENT_NAME $local_path/${chunk_pattern}-0 10

	# Chunk 2: 20 events
	validate_trace_count $EVENT_NAME $local_path/${chunk_pattern}-1 20

	shopt -u extglob
}

function rotate_timer_test ()
{
	local_path=$1
	per_pid=$2

	today=$(date +%Y%m%d)
	nr=0
	nr_iter=0
	expected_chunks=3

	# Wait for the "archives" folder to appear after the first rotation
	until [ -d $local_path ]; do
	    sleep 1
	done

	# Wait for $expected_chunks to be generated, timeout after
	# 3 * $expected_chunks * 0.5s.
	# On a laptop with an empty session, a local rotation takes about 200ms,
	# and a remote rotation takes about 600ms.
	# We currently set the timeout to 6 seconds for 3 rotations, if we get
	# errors, we can bump this value.

	until [ $nr -ge $expected_chunks ] || [ $nr_iter -ge $(($expected_chunks * 2 )) ]; do
		nr=$(ls $local_path | wc -l)
		nr_iter=$(($nr_iter+1))
		sleep 1
	done
	test $nr -ge $expected_chunks
	ok $? "Generated at least $nr chunks in $(($nr_iter))s"
	stop_lttng_tracing_ok $SESSION_NAME
	destroy_lttng_session_ok $SESSION_NAME

	i=1
	local chunk_pattern=$(get_chunk_pattern ${today})

	# Enable extglob for the use of chunk_pattern
	shopt -s extglob

	# In a per-pid setup, only the first chunk is a valid trace, the other
	# chunks should be empty folders
	if test $per_pid = 1; then
		validate_trace_empty $local_path/${chunk_pattern}-0
		nr=$(find $local_path/${chunk_pattern}-1/ | wc -l)
		# contains self and may contain ust/ subdir (local) or not (remote).
		test $nr -le 2
		ok $? "Chunk 2 is empty"
		nr=$(find $local_path/${chunk_pattern}-2/ | wc -l)
		# contains self and may contain ust/ subdir (local) or not (remote).
		test $nr -le 2
		ok $? "Chunk 3 is empty"
	else
		while [ $i -le $expected_chunks ]; do
			validate_trace_empty $local_path/${chunk_pattern}-$i
			i=$(($i+1))
		done
	fi
	shopt -u extglob
}

function trace_until_n_archives ()
{
	local produce_events=$1
	local trace_path=$2
	local target_archive_count=$3
	local trace_size_cutoff=$4
	local archive_count=0
	local trace_size=0

	diag "Waiting for $target_archive_count size-based rotations to occur"
	while [[ archive_count -lt $target_archive_count && $trace_size -lt $trace_size_cutoff ]]
	do
		archive_count=$(find "$trace_path" -mindepth 2 -maxdepth 2 -type d -path "*archives*" | wc -l)
		trace_size=$(du -b "$trace_path" | tail -n1 | cut -f1)
		$produce_events 2000
	done

	if [[ $trace_size -ge $trace_size_cutoff ]]; then
		diag "Exceeded size cutoff of $trace_size_cutoff bytes while waiting for $target_archive_count rotations"
	fi

	[[ $archive_count -eq $target_archive_count ]]
	ok $? "Found $target_archive_count trace archives resulting from trace archive rotations"
}
