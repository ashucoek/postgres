
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Test that synchronized_standby_slots supports quorum-based syntax
# (ANY N (slot1, slot2, ...)) so that logical decoding availability matches
# the commit durability guarantee of synchronous_standby_names = 'ANY ...'.
#
# Setup: a 3-node cluster with one primary, two physical standbys, and a
# logical decoding client using a failover-enabled slot.
#
#               | ----> standby1 (primary_slot_name = sb1_slot)
# primary ------|
#               | ----> standby2 (primary_slot_name = sb2_slot)
#
#   synchronous_standby_names = 'ANY 1 (standby1, standby2)'
#
# We test two scenarios:
#
# A) synchronized_standby_slots = 'sb1_slot, sb2_slot'        (ALL mode)
#    With standby1 down, logical decoding BLOCKS despite the quorum commit
#    having succeeded — this demonstrates the original limitation.
#
# B) synchronized_standby_slots = 'ANY 1 (sb1_slot, sb2_slot)' (quorum mode)
#    With standby1 down, logical decoding proceeds because sb2_slot alone
#    satisfies the quorum — this is the fix.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# ---------------------------------------------------------------------------
# 1.  Create a primary with logical replication level, autovacuum off
# ---------------------------------------------------------------------------
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 'logical');
$primary->append_conf(
	'postgresql.conf', qq{
autovacuum = off
});
$primary->start;

# Physical replication slots for the two standbys
$primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('sb1_slot');");
$primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('sb2_slot');");

# ---------------------------------------------------------------------------
# 2.  Create standby1 and standby2 from a fresh backup
# ---------------------------------------------------------------------------
my $backup_name = 'base_backup';
$primary->backup($backup_name);

my $connstr = $primary->connstr;

my $standby1 = PostgreSQL::Test::Cluster->new('standby1');
$standby1->init_from_backup(
	$primary, $backup_name,
	has_streaming => 1,
	has_restoring => 1);
$standby1->append_conf(
	'postgresql.conf', qq(
hot_standby_feedback = on
primary_slot_name = 'sb1_slot'
primary_conninfo = '$connstr dbname=postgres'
));

my $standby2 = PostgreSQL::Test::Cluster->new('standby2');
$standby2->init_from_backup(
	$primary, $backup_name,
	has_streaming => 1,
	has_restoring => 1);
$standby2->append_conf(
	'postgresql.conf', qq(
hot_standby_feedback = on
primary_slot_name = 'sb2_slot'
primary_conninfo = '$connstr dbname=postgres'
));

$standby1->start;
$standby2->start;

$primary->wait_for_replay_catchup($standby1);
$primary->wait_for_replay_catchup($standby2);

# ---------------------------------------------------------------------------
# 3.  Create a logical failover slot on the primary
# ---------------------------------------------------------------------------
$primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('logical_failover', 'test_decoding', false, false, true);"
);

# ---------------------------------------------------------------------------
# 4.  Configure quorum sync rep with ALL-mode synchronized_standby_slots
# ---------------------------------------------------------------------------
$primary->append_conf(
	'postgresql.conf', qq{
synchronous_standby_names = 'ANY 1 (standby1, standby2)'
synchronized_standby_slots = 'sb1_slot, sb2_slot'
});
$primary->reload;

$primary->wait_for_replay_catchup($standby1);
$primary->wait_for_replay_catchup($standby2);

# ---------------------------------------------------------------------------
# 5.  Confirm that quorum sync rep is active for both standbys
# ---------------------------------------------------------------------------
is( $primary->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_stat_replication WHERE sync_state = 'quorum';}
	),
	'2',
	'both standbys are in quorum sync state');

##################################################
# PART A: ALL-mode blocks even when quorum is met
##################################################

$standby1->stop;

# Commit succeeds since standby2 satisfies the quorum.
my $emit_lsn = $primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'all_mode_blocks');"
);
like($emit_lsn, qr/^[0-9A-F]+\/[0-9A-F]+$/,
	'synchronous commit succeeds with quorum (standby2 alive)');

$primary->wait_for_replay_catchup($standby2);

my $log_offset = -s $primary->logfile;

my $bg = $primary->background_psql(
	'postgres',
	on_error_stop => 0,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

$bg->query_until(
	qr/decode_start/, q(
   \echo decode_start
   SELECT pg_logical_slot_peek_changes('logical_failover', NULL, NULL);
));

# Wait for the primary to log a warning about sb1_slot not being active.
$primary->wait_for_log(
	qr/replication slot \"sb1_slot\" specified in parameter "synchronized_standby_slots" does not have active_pid/,
	$log_offset);

pass('ALL mode: logical decoding blocked by sb1_slot even though quorum commit succeeded');

# Unblock by clearing synchronized_standby_slots.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots', "''");
$primary->reload;
$bg->quit;

# Consume the change so the slot is clean for the next test.
$primary->safe_psql('postgres',
	q{SELECT pg_logical_slot_get_changes('logical_failover', NULL, NULL);});

##################################################
# PART B: ANY (quorum) mode — logical decoding proceeds
##################################################

# Switch synchronized_standby_slots to quorum mode: need only 1 of 2 slots.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'ANY 1 (sb1_slot, sb2_slot)'");
$primary->reload;

# standby1 is still down; standby2 is up.

# Emit another transactional message — commits via quorum.
$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'quorum_mode_works');"
);
$primary->wait_for_replay_catchup($standby2);

# In quorum mode, logical decoding should NOT block because sb2_slot has
# caught up and 1-of-2 is sufficient.
my $decoded = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%quorum_mode_works%';});
is($decoded, '1',
	'ANY mode: logical decoding proceeds with only sb2_slot caught up');

##################################################
# PART C: Verify backward-compat — plain list still requires ALL
##################################################

# Bring standby1 back.
$standby1->start;
$primary->wait_for_replay_catchup($standby1);

# Switch to plain list (ALL mode) with both slots.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'sb1_slot, sb2_slot'");
$primary->reload;

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'both_caught_up');"
);
$primary->wait_for_replay_catchup($standby1);
$primary->wait_for_replay_catchup($standby2);

my $decoded_bc = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%both_caught_up%';});
is($decoded_bc, '1',
	'backward-compat: plain list works when all standbys are up');

##################################################
# PART D: ANY mode — bringing standby1 back also works
##################################################

# Stop standby1 again, switch to ANY 1.
$standby1->stop;

$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'ANY 1 (sb1_slot, sb2_slot)'");
$primary->reload;

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'standby1_recovery');"
);

$primary->wait_for_replay_catchup($standby2);

# Decoding proceeds via quorum.
my $decoded_d1 = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%standby1_recovery%';});
is($decoded_d1, '1',
	'ANY mode: decoding works while standby1 is down');

# Bring standby1 back and verify decoding still works.
$standby1->start;
$primary->wait_for_replay_catchup($standby1);

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'after_recovery');"
);
$primary->wait_for_replay_catchup($standby1);
$primary->wait_for_replay_catchup($standby2);

my $decoded_d2 = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%after_recovery%';});
is($decoded_d2, '1',
	'ANY mode: decoding works after standby1 recovers');

##################################################
# PART E: Verify FIRST N syntax (treated as ALL mode)
##################################################

# FIRST 2 (sb1_slot, sb2_slot) — both standbys up, should work like ALL.
$primary->adjust_conf('postgresql.conf', 'synchronized_standby_slots',
	"'FIRST 2 (sb1_slot, sb2_slot)'");
$primary->reload;

$primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'qtest', 'first_n_test');"
);
$primary->wait_for_replay_catchup($standby1);
$primary->wait_for_replay_catchup($standby2);

my $decoded_e = $primary->safe_psql('postgres',
	q{SELECT count(*) FROM pg_logical_slot_get_changes('logical_failover', NULL, NULL)
	  WHERE data LIKE '%first_n_test%';});
is($decoded_e, '1',
	'FIRST N mode: decoding works when all standbys are up');

##################################################
# PART F: Verify GUC validation rejects bad values
##################################################

my ($result, $stdout, $stderr);

# N exceeds number of listed slots
($result, $stdout, $stderr) = $primary->psql('postgres',
	"ALTER SYSTEM SET synchronized_standby_slots = 'ANY 3 (sb1_slot, sb2_slot)';");
like($stderr, qr/ERROR/,
	'GUC rejects ANY N when N > number of listed slots');

# Missing closing parenthesis
($result, $stdout, $stderr) = $primary->psql('postgres',
	"ALTER SYSTEM SET synchronized_standby_slots = 'ANY 1 (sb1_slot, sb2_slot';");
like($stderr, qr/ERROR/,
	'GUC rejects malformed ANY syntax');

# Invalid slot name
($result, $stdout, $stderr) = $primary->psql('postgres',
	"ALTER SYSTEM SET synchronized_standby_slots = 'ANY 1 (INVALID_UPPER)';");
like($stderr, qr/ERROR/,
	'GUC rejects invalid slot name in ANY syntax');

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
$primary->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('logical_failover');");

done_testing();
