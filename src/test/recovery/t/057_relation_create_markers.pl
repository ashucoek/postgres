# Copyright (c) 2026, PostgreSQL Global Development Group

# Test cleanup of permanent relation files created by transactions that are
# still in progress when the server crashes.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('relation_create_manifests');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$node->start();

my $manifest_dir = $node->data_dir . '/pg_relcreate';

sub manifest_count
{
	my ($cluster) = @_;
	my $dir = $cluster->data_dir . '/pg_relcreate';

	return scalar(grep { $_ ne '.' && $_ ne '..' } slurp_dir($dir));
}

$node->safe_psql('postgres', 'CREATE TABLE committed_relation (a int)');
is(manifest_count($node), 0,
	'committed relation leaves no creation manifest');

my $session = $node->background_psql('postgres');
$session->query_safe('BEGIN');
my @relation_paths = split /\n/, $session->query_safe(
	'CREATE TABLE crash_aborted_relation_1 (a int); '
	  . 'CREATE TABLE crash_aborted_relation_2 (a int); '
	  . q{SELECT pg_relation_filepath('crash_aborted_relation_1') UNION ALL }
	  . q{SELECT pg_relation_filepath('crash_aborted_relation_2')});

is(scalar(grep { !-f $node->data_dir . '/' . $_ } @relation_paths), 0,
	'uncommitted relation files exist before crash');
is(manifest_count($node), 1,
	'two relations created by one transaction share one manifest');

# Move the redo pointer past the creation record.  Recovery therefore needs
# the persistent manifest; replay-local tracking of the create record is not
# sufficient.
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start();

is($node->safe_psql('postgres',
	q{SELECT to_regclass('crash_aborted_relation_1') IS NULL AND
to_regclass('crash_aborted_relation_2') IS NULL}),
	't', 'crash-aborted relations are absent from the catalog');
is(scalar(grep { -e $node->data_dir . '/' . $_ } @relation_paths), 0,
	'crash-aborted relation files are removed during recovery');
is(manifest_count($node), 0, 'processed creation manifest is removed');

my $prepared_path = $node->safe_psql(
	'postgres',
	q{BEGIN;
CREATE TABLE prepared_relation (a int);
SELECT pg_relation_filepath('prepared_relation');
PREPARE TRANSACTION 'relation_create_marker';});
ok(-f $node->data_dir . '/' . $prepared_path,
	'prepared relation file exists');
is(manifest_count($node), 1,
	'prepared relation retains its creation manifest');

$node->stop('immediate');
$node->start();

ok(-f $node->data_dir . '/' . $prepared_path,
	'prepared relation file survives recovery');
is(manifest_count($node), 1,
	'recovery retains prepared relation manifest');
$node->safe_psql('postgres',
	q{COMMIT PREPARED 'relation_create_marker'});
is($node->safe_psql('postgres',
	q{SELECT to_regclass('prepared_relation') IS NOT NULL}),
	't', 'committed prepared relation is visible');
is(manifest_count($node), 0,
	'commit prepared removes relation manifest');

$node->safe_psql(
	'postgres',
	q{BEGIN;
CREATE TABLE aborted_prepared_relation (a int);
PREPARE TRANSACTION 'relation_create_manifest_abort';});
is(manifest_count($node), 1,
	'prepared transaction to abort retains its manifest');
$node->safe_psql('postgres',
	q{ROLLBACK PREPARED 'relation_create_manifest_abort'});
is(manifest_count($node), 0,
	'rollback prepared removes relation manifest');

$node->backup('manifest_backup');
my $standby = PostgreSQL::Test::Cluster->new('relation_create_standby');
$standby->init_from_backup($node, 'manifest_backup', has_streaming => 1);
$standby->start();

my $commit_session = $node->background_psql('postgres');
$commit_session->query_safe('BEGIN');
$commit_session->query_safe(
	'CREATE TABLE standby_committed_relation_1 (a int); '
	  . 'CREATE TABLE standby_committed_relation_2 (a int)');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->wait_for_catchup($standby);
is(manifest_count($standby), 1,
	'standby uses one manifest for two relations from one transaction');
$commit_session->query_safe('COMMIT');
$node->wait_for_catchup($standby);
is(manifest_count($standby), 0,
	'commit replay removes standby relation creation manifest');

my $abort_session = $node->background_psql('postgres');
$abort_session->query_safe('BEGIN');
$abort_session->query_safe('CREATE TABLE standby_aborted_relation (a int)');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->wait_for_catchup($standby);
is(manifest_count($standby), 1,
	'standby retains manifest for an in-progress transaction');
$abort_session->query_safe('ROLLBACK');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->wait_for_catchup($standby);
is(manifest_count($standby), 0,
	'abort replay removes standby relation creation manifest');

$standby->stop();
$node->stop();
done_testing();
