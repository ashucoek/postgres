
# Copyright (c) 2026, PostgreSQL Global Development Group

# Logical replication tests for publications with EXCEPT clause
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
$node_subscriber->init;
$node_subscriber->start;

my $result;

sub test_except_root_partition
{
	my ($pubviaroot, $pubsql) = @_;
	$pubsql //=
	  "CREATE PUBLICATION tap_pub_part FOR ALL TABLES EXCEPT (TABLE root1)";
	$pubsql .= " WITH (publish_via_partition_root = $pubviaroot)";

	# If the root partitioned table is in the EXCEPT clause, all its
	# partitions are excluded from publication, regardless of the
	# publish_via_partition_root setting.
	$node_publisher->safe_psql(
		'postgres', qq(
		$pubsql;
		INSERT INTO root1 VALUES (1), (101);
	));
	$node_subscriber->safe_psql('postgres',
		"CREATE SUBSCRIPTION tap_sub_part CONNECTION '$publisher_connstr' PUBLICATION tap_pub_part"
	);
	$node_subscriber->wait_for_subscription_sync($node_publisher,
		'tap_sub_part');

	# Advance the replication slot to ignore changes generated before this point.
	$node_publisher->safe_psql('postgres',
		"SELECT slot_name FROM pg_replication_slot_advance('test_slot', pg_current_wal_lsn())"
	);
	$node_publisher->safe_psql('postgres',
		"INSERT INTO root1 VALUES (2), (102)");

	# Verify that data inserted into the partitioned table is not published when
	# it is in the EXCEPT clause.
	$result = $node_publisher->safe_psql('postgres',
		"SELECT count(*) = 0 FROM pg_logical_slot_get_binary_changes('test_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'tap_pub_part')"
	);
	$node_publisher->wait_for_catchup('tap_sub_part');

	# Verify that no rows are replicated to subscriber for root or partitions.
	foreach my $table (qw(root1 part1 part2 part2_1))
	{
		$result = $node_subscriber->safe_psql('postgres',
			"SELECT count(*) FROM $table");
		is($result, qq(0), "no rows replicated to subscriber for $table");
	}

	$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_part");
	$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_part");
}

# ============================================
# EXCEPT clause test cases for non-partitioned tables and inherited tables.
# ============================================

# Create tables on publisher
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab1 AS SELECT generate_series(1,10) AS a;
	CREATE TABLE parent (a int);
	CREATE TABLE child (b int) INHERITS (parent);
	CREATE TABLE parent1 (a int);
	CREATE TABLE child1 (b int) INHERITS (parent1);
));

# Create tables on subscriber
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab1 (a int);
	CREATE TABLE parent (a int);
	CREATE TABLE child (b int) INHERITS (parent);
	CREATE TABLE parent1 (a int);
	CREATE TABLE child1 (b int) INHERITS (parent1);
));

# Exclude tab1 (non-inheritance case), and also exclude parent and ONLY parent1
# to verify exclusion behavior for inherited tables, including the effect of
# ONLY in the EXCEPT clause.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR ALL TABLES EXCEPT (TABLE tab1, parent, only parent1)"
);

# Create a logical replication slot to help with later tests.
$node_publisher->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('test_slot', 'pgoutput')");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Check the table data does not sync for the tables specified in the EXCEPT
# clause.
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab1");
is($result, qq(0),
	'check there is no initial data copied for the tables specified in the EXCEPT clause'
);

# Insert some data into the table listed in the EXCEPT clause
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab1 VALUES(generate_series(11,20));
	INSERT INTO child VALUES(generate_series(11,20), generate_series(11,20));
));

# Verify that data inserted into a table listed in the EXCEPT clause is
# not published.
$result = $node_publisher->safe_psql('postgres',
	"SELECT count(*) = 0 FROM pg_logical_slot_get_binary_changes('test_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'tap_pub')"
);
is($result, qq(t),
	'verify no changes for table listed in the EXCEPT clause are present in the replication slot'
);

# This should be published because ONLY parent1 was specified in the
# EXCEPT clause, so the exclusion applies only to the parent table and not
# to its child.
$node_publisher->safe_psql('postgres',
	"INSERT INTO child1 VALUES(generate_series(11,20), generate_series(11,20))"
);

# Verify that data inserted into a table listed in the EXCEPT clause is
# not replicated.
$node_publisher->wait_for_catchup('tap_sub');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab1");
is($result, qq(0), 'check replicated inserts on subscriber');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM child");
is($result, qq(0), 'check replicated inserts on subscriber');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM child1");
is($result, qq(10), 'check replicated inserts on subscriber');

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab2 AS SELECT generate_series(1,10) AS a");
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab2 (a int)");

# Replace the table list in the EXCEPT clause so that only tab2 is excluded.
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub SET ALL TABLES EXCEPT (TABLE tab2)");

# Refresh the subscription so the subscriber picks up the updated
# publication definition and initiates table synchronization.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH PUBLICATION");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Verify that initial table synchronization does not occur for tables
# listed in the EXCEPT clause.
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab2");
is($result, qq(0),
	'check there is no initial data copied for the tables specified in the EXCEPT clause'
);

# Verify that table synchronization now happens for tab1. Table tab1 is
# included now since the table list of EXCEPT clause is only (tab2).
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab1");
is($result, qq(20),
	'check that the data is copied as the tab1 is removed from EXCEPT clause'
);

# cleanup
$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION tap_sub;
	TRUNCATE TABLE tab1;
	DROP TABLE parent, parent1, child, child1, tab2;
));
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tap_pub;
	TRUNCATE TABLE tab1;
    DROP TABLE parent, parent1, child, child1, tab2;
));

# ============================================
# EXCEPT clause test cases for partitioned tables
# ============================================
# Setup partitioned table and partitions on the publisher that map to normal
# tables on the subscriber.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE root1(a int) PARTITION BY RANGE(a);
	CREATE TABLE part1 PARTITION OF root1 FOR VALUES FROM (0) TO (100);
	CREATE TABLE part2 PARTITION OF root1 FOR VALUES FROM (100) TO (200) PARTITION BY RANGE(a);
	CREATE TABLE part2_1 PARTITION OF part2 FOR VALUES FROM (100) TO (150);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE root1(a int);
	CREATE TABLE part1(a int);
	CREATE TABLE part2(a int);
	CREATE TABLE part2_1(a int);
));

# Validate the behaviour with both publish_via_partition_root as true and false
test_except_root_partition('false');
test_except_root_partition('true');

# Same validation using TABLES IN SCHEMA instead of FOR ALL TABLES.
my $schema_pub =
  "CREATE PUBLICATION tap_pub_part FOR TABLES IN SCHEMA public EXCEPT (TABLE public.root1)";
test_except_root_partition('false', $schema_pub);
test_except_root_partition('true', $schema_pub);

# ============================================
# EXCEPT test cases for TABLES IN SCHEMA
# ============================================

# Create a dedicated schema with two tables: one to be published and one to be
# excluded.  Also create inherited tables to verify ONLY semantics.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE SCHEMA sch1;
	CREATE TABLE sch1.tab_published AS SELECT generate_series(1,5) AS a;
	CREATE TABLE sch1.tab_excluded AS SELECT generate_series(1,5) AS a;
	CREATE TABLE sch1.parent (a int);
	CREATE TABLE sch1.child (b int) INHERITS (sch1.parent);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE SCHEMA sch1;
	CREATE TABLE sch1.tab_published (a int);
	CREATE TABLE sch1.tab_excluded (a int);
	CREATE TABLE sch1.parent (a int);
	CREATE TABLE sch1.child (b int) INHERITS (sch1.parent);
));

# Basic test: initial sync respects EXCEPT.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION sch_pub FOR TABLES IN SCHEMA sch1 EXCEPT (TABLE sch1.tab_excluded)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sch_sub CONNECTION '$publisher_connstr' PUBLICATION sch_pub"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sch_sub');

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_published");
is($result, qq(5),
	'TABLES IN SCHEMA EXCEPT: initial sync copies included table');
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_excluded");
is($result, qq(0),
	'TABLES IN SCHEMA EXCEPT: initial sync skips excluded table');

# DML: only the included table should be replicated.
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO sch1.tab_published VALUES (6);
	INSERT INTO sch1.tab_excluded VALUES (6);
));
$node_publisher->wait_for_catchup('sch_sub');

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_published");
is($result, qq(6),
	'TABLES IN SCHEMA EXCEPT: DML on included table is replicated');
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_excluded");
is($result, qq(0),
	'TABLES IN SCHEMA EXCEPT: DML on excluded table is not replicated');

$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sch_sub');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION sch_pub');

# Inherited tables: excluding the parent (without ONLY) also excludes the child.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION sch_pub FOR TABLES IN SCHEMA sch1 EXCEPT (TABLE sch1.parent)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sch_sub CONNECTION '$publisher_connstr' PUBLICATION sch_pub"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sch_sub');

$node_publisher->safe_psql('postgres',
	"INSERT INTO sch1.child VALUES (generate_series(1,5), generate_series(1,5))"
);
$node_publisher->wait_for_catchup('sch_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM sch1.child");
is($result, qq(0),
	'TABLES IN SCHEMA EXCEPT: excluding parent (without ONLY) also excludes child'
);

$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sch_sub');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION sch_pub');

# Test that EXCEPT (TABLE ONLY parent) excludes only the parent itself, not its
# child.  Truncate child first so rows from the previous test are not copied by
# the initial table sync of the next subscription.
$node_publisher->safe_psql('postgres', 'TRUNCATE sch1.child');
$node_subscriber->safe_psql('postgres', 'TRUNCATE sch1.child');
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION sch_pub FOR TABLES IN SCHEMA sch1 EXCEPT (TABLE ONLY sch1.parent)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sch_sub CONNECTION '$publisher_connstr' PUBLICATION sch_pub"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sch_sub');

$node_publisher->safe_psql('postgres',
	"INSERT INTO sch1.child VALUES (generate_series(1,5), generate_series(1,5))"
);
$node_publisher->wait_for_catchup('sch_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM sch1.child");
is($result, qq(5),
	'TABLES IN SCHEMA EXCEPT: ONLY parent in EXCEPT does not exclude child');

$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sch_sub');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION sch_pub');

# ============================================
# ALTER PUBLICATION EXCEPT for TABLES IN SCHEMA
# ============================================

# Truncate subscriber tables to remove data accumulated from previous tests.
$node_subscriber->safe_psql('postgres',
	'TRUNCATE sch1.tab_published, sch1.tab_excluded, sch1.parent, sch1.child');

# ADD: add a schema with an excepted table; verify the except entry takes effect.
$node_publisher->safe_psql('postgres', "CREATE PUBLICATION sch_pub");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION sch_pub ADD TABLES IN SCHEMA sch1 EXCEPT (TABLE sch1.tab_excluded)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sch_sub CONNECTION '$publisher_connstr' PUBLICATION sch_pub"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sch_sub');

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_published");
is($result, qq(6),
	'ALTER ... ADD TABLES IN SCHEMA EXCEPT: included table synced');
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_excluded");
is($result, qq(0),
	'ALTER ... ADD TABLES IN SCHEMA EXCEPT: excluded table not synced');

# SET: replace the except list; tab_excluded is now included and tab_published is excluded.
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION sch_pub SET TABLES IN SCHEMA sch1 EXCEPT (TABLE sch1.tab_published)"
);
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION sch_sub REFRESH PUBLICATION");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sch_sub');

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO sch1.tab_published VALUES (7);
	INSERT INTO sch1.tab_excluded VALUES (7);
));
$node_publisher->wait_for_catchup('sch_sub');

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_excluded WHERE a = 7");
is($result, qq(1),
	'ALTER ... SET TABLES IN SCHEMA EXCEPT: newly included table is replicated'
);
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_published WHERE a = 7");
is($result, qq(0),
	'ALTER ... SET TABLES IN SCHEMA EXCEPT: now-excluded table is not replicated'
);

# SET without EXCEPT: clears the except list; both tables are now published.
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION sch_pub SET TABLES IN SCHEMA sch1");
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION sch_sub REFRESH PUBLICATION");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sch_sub');

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO sch1.tab_published VALUES (8);
	INSERT INTO sch1.tab_excluded VALUES (8);
));
$node_publisher->wait_for_catchup('sch_sub');

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_published WHERE a = 8");
is($result, qq(1),
	'ALTER ... SET TABLES IN SCHEMA (no EXCEPT): tab_published replicated after except list cleared'
);
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM sch1.tab_excluded WHERE a = 8");
is($result, qq(1),
	'ALTER ... SET TABLES IN SCHEMA (no EXCEPT): tab_excluded replicated after except list cleared'
);

$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION sch_sub');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION sch_pub');

# Cleanup schema tables before the multi-publication section.
$node_publisher->safe_psql('postgres', 'DROP SCHEMA sch1 CASCADE');
$node_subscriber->safe_psql('postgres', 'DROP SCHEMA sch1 CASCADE');

# ============================================
# Test when a subscription is subscribing to multiple publications
# ============================================

# OK when a table is excluded by pub1 EXCEPT clause, but it is included by pub2
# FOR TABLE.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub1 FOR ALL TABLES EXCEPT (TABLE tab1);
	CREATE PUBLICATION tap_pub2 FOR TABLE tab1;
	INSERT INTO tab1 VALUES(1);
));
$node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', qq(INSERT INTO tab1 VALUES(2)));
$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_publisher->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY a");
is( $result, qq(1
2),
	"check replication of a table in the EXCEPT clause of one publication but included by another"
);
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tap_pub2;
	TRUNCATE tab1;
));
$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION tap_sub');
$node_subscriber->safe_psql('postgres', qq(TRUNCATE tab1));

# OK when a table is excluded by pub1 EXCEPT clause, but it is included by pub2
# FOR ALL TABLES.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub2 FOR ALL TABLES;
	INSERT INTO tab1 VALUES(1);
));
$node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', qq(INSERT INTO tab1 VALUES(2)));
$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_publisher->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY a");
is( $result, qq(1
2),
	"check replication of a table in the EXCEPT clause of one publication but included by another"
);

$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION tap_sub');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION tap_pub1');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION tap_pub2');

# OK when a table is excluded by a TABLES IN SCHEMA EXCEPT publication,
# but is included by another publication.
$node_publisher->safe_psql('postgres', 'TRUNCATE tab1');
$node_subscriber->safe_psql('postgres', 'TRUNCATE tab1');

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub1 FOR TABLES IN SCHEMA public EXCEPT (TABLE public.tab1);
	CREATE PUBLICATION tap_pub2 FOR TABLE tab1;
	INSERT INTO tab1 VALUES(1);
));
$node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', qq(INSERT INTO tab1 VALUES(2)));
$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY a");
is( $result, qq(1
2),
	"TABLES IN SCHEMA EXCEPT: table excluded in schema pub but included by another pub is replicated"
);

$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION tap_sub');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION tap_pub1');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION tap_pub2');

$node_publisher->stop('fast');

done_testing();
