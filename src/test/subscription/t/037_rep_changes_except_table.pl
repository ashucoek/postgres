
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Logical replication tests for EXCEPT TABLE publications
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

# Initialize subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

my $result;

sub test_except_root_partition
{
	my $pubviaroot = @_;

	# When the root partitioned table is listed in the EXCEPT clause,
	# all its partitions are not published.
	$node_publisher->safe_psql(
		'postgres', qq(
		CREATE PUBLICATION tap_pub_part FOR ALL TABLES EXCEPT TABLE (sch1.t1) WITH (publish_via_partition_root = $pubviaroot);
		INSERT INTO sch1.t1 VALUES (1), (101), (151);
	));
	$node_subscriber->safe_psql('postgres',
		"CREATE SUBSCRIPTION tap_sub_part CONNECTION '$publisher_connstr' PUBLICATION tap_pub_part"
	);
	$node_subscriber->wait_for_subscription_sync($node_publisher,
		'tap_sub_part');
	$node_publisher->safe_psql('postgres',
		"SELECT slot_name FROM pg_replication_slot_advance('test_slot', pg_current_wal_lsn());"
	);
	$node_publisher->safe_psql('postgres',
		"INSERT INTO sch1.t1 VALUES (2), (102), (152)");

	# Verify that data inserted into the partitioned table is not published when
	# it is in the EXCEPT clause.
	$result = $node_publisher->safe_psql('postgres',
		"SELECT count(*) = 0 FROM pg_logical_slot_get_binary_changes('test_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'tap_pub_part')"
	);
	$node_publisher->wait_for_catchup('tap_sub_part');

	# Check that no rows are replicated to subscriber
	$result =
	  $node_subscriber->safe_psql('postgres', "SELECT * FROM sch1.t1");
	is($result, qq(), 'check rows on root table');

	$result =
	  $node_subscriber->safe_psql('postgres', "SELECT * FROM sch1.part1");
	is($result, qq(), 'check rows on table sch1.part1');

	$result =
	  $node_subscriber->safe_psql('postgres', "SELECT * FROM sch1.part2");
	is($result, qq(), 'check rows on table sch1.part2');

	$result =
	  $node_subscriber->safe_psql('postgres', "SELECT * FROM sch1.part2_1");
	is($result, qq(), 'check rows on table sch1.part2_1');

	$result =
	  $node_subscriber->safe_psql('postgres', "SELECT * FROM sch1.part2_2");
	is($result, qq(), 'check rows on table sch1.part2_2');

	$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_part");
	$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_part;");
}

# ============================================
# EXCEPT TABLE test cases for normal tables
# ============================================
# Create schemas and tables on publisher
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE SCHEMA sch1;
	CREATE TABLE sch1.tab1 AS SELECT generate_series(1,10) AS a;
));

# Create schemas and tables on subscriber
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE SCHEMA sch1;
	CREATE TABLE sch1.tab1 (a int);
));

# Setup logical replication, and create a logical replication slot to help with
# later tests.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_schema FOR ALL TABLES EXCEPT TABLE (sch1.tab1)"
);

$node_publisher->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('test_slot', 'pgoutput')");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_schema CONNECTION '$publisher_connstr' PUBLICATION tap_pub_schema"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher,
	'tap_sub_schema');

# Check the table data does not sync for the tables specified in EXCEPT clause
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM sch1.tab1");
is($result, qq(0),
	'check there is no initial data copied for the tables specified in the except clause'
);

# Insert some data into the table listed in the EXCEPT clause
$node_publisher->safe_psql('postgres',
	"INSERT INTO sch1.tab1 VALUES(generate_series(11,20))");

# Verify that data inserted into a table listed in the EXCEPT clause is not
# published.
$result = $node_publisher->safe_psql('postgres',
	"SELECT count(*) = 0 FROM pg_logical_slot_get_binary_changes('test_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'tap_sub_schema')"
);
is($result, qq(t),
	'verify no changes for table listed in the EXCEPT clause are present in the replication slot'
);

# Verify that data inserted into a table listed in the EXCEPT clause is not
# replicated.
$node_publisher->wait_for_catchup('tap_sub_schema');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM sch1.tab1");
is($result, qq(0), 'check replicated inserts on subscriber');

# cleanup
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_schema");
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tap_pub_schema;
	TRUNCATE TABLE sch1.tab1;
));
$node_subscriber->safe_psql('postgres', "TRUNCATE TABLE sch1.tab1");

# ============================================
# EXCEPT TABLE test cases for partitioned tables
# Check behavior of EXCEPT TABLE with publish_via_partition_root on a
# partitioned table and its partitions.
# ============================================
# Setup partitioned table and partitions on the publisher that map to normal
# tables on the subscriber
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE sch1.t1(a int) PARTITION BY RANGE(a);
	CREATE TABLE sch1.part1 PARTITION OF sch1.t1 FOR VALUES FROM (0) TO (100);
	CREATE TABLE sch1.part2 PARTITION OF sch1.t1 FOR VALUES FROM (100) TO (200) PARTITION BY RANGE(a);;
	CREATE TABLE sch1.part2_1 PARTITION OF sch1.part2 FOR VALUES FROM (100) TO (150);
	CREATE TABLE sch1.part2_2 PARTITION OF sch1.part2 FOR VALUES FROM (150) TO (200);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE sch1.t1(a int);
	CREATE TABLE sch1.part1(a int);
	CREATE TABLE sch1.part2(a int);
	CREATE TABLE sch1.part2_1(a int);
	CREATE TABLE sch1.part2_2(a int);
));

test_except_root_partition('false');
test_except_root_partition('true');

$node_publisher->safe_psql('postgres',
	"SELECT slot_name FROM pg_replication_slot_advance('test_slot', pg_current_wal_lsn());"
);

# ============================================
# Test when a subscription is subscribing to multiple publications
# ============================================
# ERROR if subscribing to multiple publications having EXCEPT TABLE.
my ($stdout, $stderr);

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub1 FOR ALL TABLES EXCEPT (sch1.tab1);
	CREATE PUBLICATION tap_pub2 FOR ALL TABLES EXCEPT (sch1.t1);
));

($result, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
like(
	$stderr,
	qr/ERROR:  cannot combine publications "tap_pub1", "tap_pub2" with an EXCEPT TABLE clause/,
	'subscription with multiple EXCEPT TABLE publication');

$node_publisher->safe_psql('postgres', 'DROP PUBLICATION tap_pub2');

# OK when a table is excluded by pub1 EXCEPT TABLE, but it is included by pub2
# FOR TABLE
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub2 FOR TABLE sch1.tab1;
	INSERT INTO sch1.tab1 VALUES(1);
));
$node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', qq(INSERT INTO sch1.tab1 VALUES(2)));
$node_publisher->wait_for_catchup('tap_sub');

$result = $node_publisher->safe_psql('postgres',
	"SELECT * FROM sch1.tab1 ORDER BY a");
is( $result, qq(1
2),
	"check replication of a table in the EXCEPT clause of one publication but included by another"
);
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tap_pub2;
	TRUNCATE sch1.tab1;
));
$node_subscriber->safe_psql('postgres', qq(TRUNCATE sch1.tab1));

# OK when a table is excluded by pub1 EXCEPT TABLE, but it is included by pub2
# FOR ALL TABLES
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub2 FOR ALL TABLES;
	INSERT INTO sch1.tab1 VALUES(1);
));
$node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', qq(INSERT INTO sch1.tab1 VALUES(2)));
$node_publisher->wait_for_catchup('tap_sub');

$result = $node_publisher->safe_psql('postgres',
	"SELECT * FROM sch1.tab1 ORDER BY a");
is( $result, qq(1
2),
	"check replication of a table in the EXCEPT clause of one publication but included by another"
);

# ERROR if ALTER SUBSCRIPTION ... REFRESH PUBLICATION causes the
# subscription to end up with multiple publications having EXCEPT TABLE.
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tap_pub2;
	CREATE PUBLICATION tap_pub2 FOR ALL TABLES EXCEPT (sch1.t1);
));

($result, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH PUBLICATION");
like(
	$stderr,
	qr/ERROR:  cannot combine publications "tap_pub1", "tap_pub2" with an EXCEPT TABLE clause/,
	'subscription with multiple EXCEPT TABLE publication');

$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION tap_sub');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION tap_pub1');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION tap_pub2');

$node_publisher->stop('fast');

done_testing();
