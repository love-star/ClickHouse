#!/usr/bin/env bash
# Tags: long, distributed, no-msan, no-replicated-database
# Tag no-msan: issue 21600
# Tag no-replicated-database: ON CLUSTER is not allowed

CLICKHOUSE_CLIENT_SERVER_LOGS_LEVEL=fatal

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

TMP_OUT=$(mktemp "$CURDIR/01175_distributed_ddl_output_mode_long.XXXXXX")
trap 'rm -f ${TMP_OUT:?}' EXIT

TIMEOUT=300
MIN_TIMEOUT=1

# We execute a distributed DDL query with timeout 1 to check that one host is unavailable and will time out and other complete successfully.
# But sometimes one second is not enough even for healthy host to succeed. Repeat the test in this case.
function run_until_out_contains()
{
    PATTERN=$1
    shift

    for ((i=MIN_TIMEOUT; i<33; i=i*2))
    do
        "$@" --distributed_ddl_task_timeout="$i" > "$TMP_OUT" 2>&1
        if grep -q "$PATTERN" "$TMP_OUT"
        then
            cat "$TMP_OUT" | sed "s/distributed_ddl_task_timeout (=$i)/distributed_ddl_task_timeout (=$MIN_TIMEOUT)/g"
            break
        fi
    done
}

RAND_COMMENT="01175_DDL_$CLICKHOUSE_DATABASE"
LOG_COMMENT="${CLICKHOUSE_LOG_COMMENT}_$RAND_COMMENT"

CLICKHOUSE_CLIENT_WITH_SETTINGS=${CLICKHOUSE_CLIENT/--log_comment ${CLICKHOUSE_LOG_COMMENT}/--log_comment ${LOG_COMMENT}}
CLICKHOUSE_CLIENT_WITH_SETTINGS+=" --output_format_parallel_formatting=0 --database_atomic_wait_for_drop_and_detach_synchronously=0 "

CLIENT=${CLICKHOUSE_CLIENT_WITH_SETTINGS}
CLIENT+=" --distributed_ddl_task_timeout=$TIMEOUT "

$CLICKHOUSE_CLIENT -q "drop table if exists none;"
$CLICKHOUSE_CLIENT -q "drop table if exists throw;"
$CLICKHOUSE_CLIENT -q "drop table if exists null_status;"
$CLICKHOUSE_CLIENT -q "drop table if exists never_throw;"

$CLIENT --distributed_ddl_output_mode=none -q "select value from system.settings where name='distributed_ddl_output_mode';"
# Ok
$CLIENT --distributed_ddl_output_mode=none -q "create table none on cluster test_shard_localhost (n int) engine=Memory;"
# Table exists
$CLIENT --distributed_ddl_output_mode=none -q "create table none on cluster test_shard_localhost (n int) engine=Memory;" 2>&1 | sed "s/DB::Exception/Error/g" | sed "s/ (version.*)//"
# Timeout

run_until_out_contains 'not finished on 1 ' $CLICKHOUSE_CLIENT_WITH_SETTINGS --distributed_ddl_output_mode=none -q "drop table if exists none on cluster test_unavailable_shard;" 2>&1 | sed "s/DB::Exception/Error/g" | sed "s/ (version.*)//" | sed "s/Distributed DDL task .* is not finished/Distributed DDL task <task> is not finished/" | sed "s/for .* seconds/for <sec> seconds/"


$CLIENT --distributed_ddl_output_mode=throw -q "select value from system.settings where name='distributed_ddl_output_mode';"
$CLIENT --distributed_ddl_output_mode=throw -q "create table throw on cluster test_shard_localhost (n int) engine=Memory;"
$CLIENT --distributed_ddl_output_mode=throw -q "create table throw on cluster test_shard_localhost (n int) engine=Memory format Null;" 2>&1 | sed "s/DB::Exception/Error/g" | sed "s/ (version.*)//"

# ┌─host──────┬─port─┬─status─┬─error─┬─num_hosts_remaining─┬─num_hosts_active─┐
# │ localhost │ 9000 │      0 │       │                   1 │                0 │
# └───────────┴──────┴────────┴───────┴─────────────────────┴──────────────────┘
run_until_out_contains '9000	0		1	0' $CLICKHOUSE_CLIENT_WITH_SETTINGS --distributed_ddl_output_mode=throw -q "drop table if exists throw on cluster test_unavailable_shard;" 2>&1 | sed "s/DB::Exception/Error/g" | sed "s/ (version.*)//" | sed "s/Distributed DDL task .* is not finished/Distributed DDL task <task> is not finished/" | sed "s/for .* seconds/for <sec> seconds/"


$CLIENT --distributed_ddl_output_mode=null_status_on_timeout -q "select value from system.settings where name='distributed_ddl_output_mode';"
$CLIENT --distributed_ddl_output_mode=null_status_on_timeout -q "create table null_status on cluster test_shard_localhost (n int) engine=Memory;"
$CLIENT --distributed_ddl_output_mode=null_status_on_timeout -q "create table null_status on cluster test_shard_localhost (n int) engine=Memory format Null;" 2>&1 | sed "s/DB::Exception/Error/g" | sed "s/ (version.*)//"

run_until_out_contains '9000	0		' $CLICKHOUSE_CLIENT_WITH_SETTINGS --distributed_ddl_output_mode=null_status_on_timeout -q "drop table if exists null_status on cluster test_unavailable_shard;"


$CLIENT --distributed_ddl_output_mode=never_throw -q "select value from system.settings where name='distributed_ddl_output_mode';"
$CLIENT --distributed_ddl_output_mode=never_throw -q "create table never_throw on cluster test_shard_localhost (n int) engine=Memory;"
$CLIENT --distributed_ddl_output_mode=never_throw -q "create table never_throw on cluster test_shard_localhost (n int) engine=Memory;" 2>&1 | sed "s/DB::Exception/Error/g" | sed "s/ (version.*)//"

run_until_out_contains '9000	0		' $CLICKHOUSE_CLIENT_WITH_SETTINGS --distributed_ddl_output_mode=never_throw -q "drop table if exists never_throw on cluster test_unavailable_shard;"


$CLICKHOUSE_CLIENT -q "drop table if exists none;"
$CLICKHOUSE_CLIENT -q "drop table if exists throw;"
$CLICKHOUSE_CLIENT -q "drop table if exists null_status;"
$CLICKHOUSE_CLIENT -q "drop table if exists never_throw;"

$CLICKHOUSE_CLIENT -q "select 'distributed_ddl_queue'"
$CLICKHOUSE_CLIENT -q "select entry_version, initiator_host, initiator_port, cluster, replaceRegexpOne(query, 'UUID \'[0-9a-f\-]{36}\' ', ''), abs(query_create_time - now()) < 600,
    host, port, status, exception_code, replace(replaceRegexpOne(exception_text, ' \(version.*', ''), 'Exception', 'Error'), abs(query_finish_time - query_create_time - query_duration_ms/1000) <= 1 , query_duration_ms < 600000
    from system.distributed_ddl_queue
    where arrayExists((key, val) -> key='log_comment' and val like '%$RAND_COMMENT%', mapKeys(settings), mapValues(settings))
    and arrayExists((key, val) -> key='distributed_ddl_task_timeout' and val in ('$TIMEOUT', '$MIN_TIMEOUT'), mapKeys(settings), mapValues(settings))
    order by entry, host, port, exception_code" | sed 's/.ec2.internal//g'  # running job in docker with --network=host in CI leads to hostname=localhost.ec2.internal - sed is to align it with reference output
