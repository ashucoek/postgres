/*-------------------------------------------------------------------------
 *
 * pg_publication.c
 *		publication C API manipulation
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/catalog/pg_publication.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_type.h"
#include "commands/publicationcmds.h"
#include "funcapi.h"
#include "replication/logical.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/* Records association between publication and published table */
typedef struct
{
	Oid			relid;			/* OID of published table */
	Oid			pubid;			/* OID of publication that publishes this
								 * table. */
} published_rel;

/*
 * Check if relation can be in given publication and throws appropriate
 * error if not.
 */
static void
check_publication_add_relation(Relation targetrel)
{
	/* Must be a regular or partitioned table */
	if (RelationGetForm(targetrel)->relkind != RELKIND_RELATION &&
		RelationGetForm(targetrel)->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add relation \"%s\" to publication",
						RelationGetRelationName(targetrel)),
				 errdetail_relkind_not_supported(RelationGetForm(targetrel)->relkind)));

	/* Can't be system table */
	if (IsCatalogRelation(targetrel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add relation \"%s\" to publication",
						RelationGetRelationName(targetrel)),
				 errdetail("This operation is not supported for system tables.")));

	/* UNLOGGED and TEMP relations cannot be part of publication. */
	if (targetrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add relation \"%s\" to publication",
						RelationGetRelationName(targetrel)),
				 errdetail("This operation is not supported for temporary tables.")));
	else if (targetrel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add relation \"%s\" to publication",
						RelationGetRelationName(targetrel)),
				 errdetail("This operation is not supported for unlogged tables.")));
}

/*
 * Check if schema can be in given publication and throw appropriate error if
 * not.
 */
static void
check_publication_add_schema(Oid schemaid)
{
	/* Can't be system namespace */
	if (IsCatalogNamespace(schemaid) || IsToastNamespace(schemaid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add schema \"%s\" to publication",
						get_namespace_name(schemaid)),
				 errdetail("This operation is not supported for system schemas.")));

	/* Can't be temporary namespace */
	if (isAnyTempNamespace(schemaid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot add schema \"%s\" to publication",
						get_namespace_name(schemaid)),
				 errdetail("Temporary schemas cannot be replicated.")));
}

/*
 * Returns if relation represented by oid and Form_pg_class entry
 * is publishable.
 *
 * Does same checks as check_publication_add_relation() above except for
 * RELKIND_SEQUENCE, but does not need relation to be opened and also does
 * not throw errors. Here, the additional check is to support ALL SEQUENCES
 * publication.
 *
 * XXX  This also excludes all tables with relid < FirstNormalObjectId,
 * ie all tables created during initdb.  This mainly affects the preinstalled
 * information_schema.  IsCatalogRelationOid() only excludes tables with
 * relid < FirstUnpinnedObjectId, making that test rather redundant,
 * but really we should get rid of the FirstNormalObjectId test not
 * IsCatalogRelationOid.  We can't do so today because we don't want
 * information_schema tables to be considered publishable; but this test
 * is really inadequate for that, since the information_schema could be
 * dropped and reloaded and then it'll be considered publishable.  The best
 * long-term solution may be to add a "relispublishable" bool to pg_class,
 * and depend on that instead of OID checks.
 */
static bool
is_publishable_class(Oid relid, Form_pg_class reltuple)
{
	return (reltuple->relkind == RELKIND_RELATION ||
			reltuple->relkind == RELKIND_PARTITIONED_TABLE ||
			reltuple->relkind == RELKIND_SEQUENCE) &&
		!IsCatalogRelationOid(relid) &&
		reltuple->relpersistence == RELPERSISTENCE_PERMANENT &&
		relid >= FirstNormalObjectId;
}

/*
 * Another variant of is_publishable_class(), taking a Relation.
 */
bool
is_publishable_relation(Relation rel)
{
	return is_publishable_class(RelationGetRelid(rel), rel->rd_rel);
}

/*
 * SQL-callable variant of the above
 *
 * This returns null when the relation does not exist.  This is intended to be
 * used for example in psql to avoid gratuitous errors when there are
 * concurrent catalog changes.
 */
Datum
pg_relation_is_publishable(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	bool		result;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		PG_RETURN_NULL();
	result = is_publishable_class(relid, (Form_pg_class) GETSTRUCT(tuple));
	ReleaseSysCache(tuple);
	PG_RETURN_BOOL(result);
}

/*
 * Returns true if the ancestor is in the list of published relations.
 * Otherwise, returns false.
 */
static bool
is_ancestor_member_tableinfos(Oid ancestor, List *table_infos)
{
	ListCell   *lc;

	foreach(lc, table_infos)
	{
		Oid			relid = ((published_rel *) lfirst(lc))->relid;

		if (relid == ancestor)
			return true;
	}

	return false;
}

/*
 * Filter out the partitions whose parent tables are also present in the list.
 */
static void
filter_partitions(List *table_infos)
{
	ListCell   *lc;

	foreach(lc, table_infos)
	{
		bool		skip = false;
		List	   *ancestors = NIL;
		ListCell   *lc2;
		published_rel *table_info = (published_rel *) lfirst(lc);

		if (get_rel_relispartition(table_info->relid))
			ancestors = get_partition_ancestors(table_info->relid);

		foreach(lc2, ancestors)
		{
			Oid			ancestor = lfirst_oid(lc2);

			if (is_ancestor_member_tableinfos(ancestor, table_infos))
			{
				skip = true;
				break;
			}
		}

		if (skip)
			table_infos = foreach_delete_current(table_infos, lc);
	}
}

/*
 * Returns true if any schema is associated with the publication, false if no
 * schema is associated with the publication.
 */
bool
is_schema_publication(Oid pubid)
{
	Relation	pubschsrel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;
	bool		result = false;

	pubschsrel = table_open(PublicationNamespaceRelationId, AccessShareLock);
	ScanKeyInit(&scankey,
				Anum_pg_publication_namespace_pnpubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(pubid));

	scan = systable_beginscan(pubschsrel,
							  PublicationNamespacePnnspidPnpubidIndexId,
							  true, NULL, 1, &scankey);
	tup = systable_getnext(scan);
	result = HeapTupleIsValid(tup);

	systable_endscan(scan);
	table_close(pubschsrel, AccessShareLock);

	return result;
}

/*
 * Returns true if the relation has column list associated with the
 * publication, false otherwise.
 *
 * If a column list is found, the corresponding bitmap is returned through the
 * cols parameter, if provided. The bitmap is constructed within the given
 * memory context (mcxt).
 */
bool
check_and_fetch_column_list(Publication *pub, Oid relid, MemoryContext mcxt,
							Bitmapset **cols)
{
	HeapTuple	cftuple;
	bool		found = false;

	if (pub->alltables)
		return false;

	cftuple = SearchSysCache2(PUBLICATIONRELMAP,
							  ObjectIdGetDatum(relid),
							  ObjectIdGetDatum(pub->oid));
	if (HeapTupleIsValid(cftuple))
	{
		Datum		cfdatum;
		bool		isnull;

		/* Lookup the column list attribute. */
		cfdatum = SysCacheGetAttr(PUBLICATIONRELMAP, cftuple,
								  Anum_pg_publication_rel_prattrs, &isnull);

		/* Was a column list found? */
		if (!isnull)
		{
			/* Build the column list bitmap in the given memory context. */
			if (cols)
				*cols = pub_collist_to_bitmapset(*cols, cfdatum, mcxt);

			found = true;
		}

		ReleaseSysCache(cftuple);
	}

	return found;
}

/*
 * Gets the relations based on the publication partition option for a specified
 * relation.
 */
List *
GetPubPartitionOptionRelations(List *result, PublicationPartOpt pub_partopt,
							   Oid relid)
{
	if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE &&
		pub_partopt != PUBLICATION_PART_ROOT)
	{
		List	   *all_parts = find_all_inheritors(relid, NoLock,
													NULL);

		if (pub_partopt == PUBLICATION_PART_ALL)
			result = list_concat(result, all_parts);
		else if (pub_partopt == PUBLICATION_PART_LEAF)
		{
			ListCell   *lc;

			foreach(lc, all_parts)
			{
				Oid			partOid = lfirst_oid(lc);

				if (get_rel_relkind(partOid) != RELKIND_PARTITIONED_TABLE)
					result = lappend_oid(result, partOid);
			}
		}
		else
			Assert(false);
	}
	else
		result = lappend_oid(result, relid);

	return result;
}

/*
 * Returns the relid of the topmost ancestor that is published via this
 * publication if any and set its ancestor level to ancestor_level,
 * otherwise returns InvalidOid.
 *
 * The ancestor_level value allows us to compare the results for multiple
 * publications, and decide which value is higher up.
 *
 * Note that the list of ancestors should be ordered such that the topmost
 * ancestor is at the end of the list.
 */
Oid
GetTopMostAncestorInPublication(Oid puboid, List *ancestors, int *ancestor_level)
{
	ListCell   *lc;
	Oid			topmost_relid = InvalidOid;
	int			level = 0;

	/*
	 * Find the "topmost" ancestor that is in this publication.
	 */
	foreach(lc, ancestors)
	{
		Oid			ancestor = lfirst_oid(lc);
		List	   *apubids = NIL;
		List	   *aschemaPubids = NIL;

		GetRelationPublications(ancestor, &apubids, NULL);

		level++;

		if (list_member_oid(apubids, puboid))
		{
			topmost_relid = ancestor;

			if (ancestor_level)
				*ancestor_level = level;
		}
		else
		{
			aschemaPubids = GetSchemaPublications(get_rel_namespace(ancestor));
			if (list_member_oid(aschemaPubids, puboid))
			{
				topmost_relid = ancestor;

				if (ancestor_level)
					*ancestor_level = level;
			}
		}

		list_free(apubids);
		list_free(aschemaPubids);
	}

	return topmost_relid;
}

/*
 * attnumstoint2vector
 *		Convert a Bitmapset of AttrNumbers into an int2vector.
 *
 * AttrNumber numbers are 0-based, i.e., not offset by
 * FirstLowInvalidHeapAttributeNumber.
 */
static int2vector *
attnumstoint2vector(Bitmapset *attrs)
{
	int2vector *result;
	int			n = bms_num_members(attrs);
	int			i = -1;
	int			j = 0;

	result = buildint2vector(NULL, n);

	while ((i = bms_next_member(attrs, i)) >= 0)
	{
		Assert(i <= PG_INT16_MAX);

		result->values[j++] = (int16) i;
	}

	return result;
}

/*
 * Insert new publication / relation mapping.
 */
ObjectAddress
publication_add_relation(Oid pubid, PublicationRelInfo *pri,
						 bool if_not_exists)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_publication_rel];
	bool		nulls[Natts_pg_publication_rel];
	Relation	targetrel = pri->relation;
	Oid			relid = RelationGetRelid(targetrel);
	Oid			pubreloid;
	Bitmapset  *attnums;
	Publication *pub = GetPublication(pubid);
	ObjectAddress myself,
				referenced;
	List	   *relids = NIL;
	int			i;

	rel = table_open(PublicationRelRelationId, RowExclusiveLock);

	/*
	 * Check for duplicates. Note that this does not really prevent
	 * duplicates, it's here just to provide nicer error message in common
	 * case. The real protection is the unique key on the catalog.
	 */
	if (SearchSysCacheExists2(PUBLICATIONRELMAP, ObjectIdGetDatum(relid),
							  ObjectIdGetDatum(pubid)))
	{
		table_close(rel, RowExclusiveLock);

		if (if_not_exists)
			return InvalidObjectAddress;

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("relation \"%s\" is already member of publication \"%s\"",
						RelationGetRelationName(targetrel), pub->name)));
	}

	check_publication_add_relation(targetrel);

	/* Validate and translate column names into a Bitmapset of attnums. */
	attnums = pub_collist_validate(pri->relation, pri->columns);

	/* Form a tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	pubreloid = GetNewOidWithIndex(rel, PublicationRelObjectIndexId,
								   Anum_pg_publication_rel_oid);
	values[Anum_pg_publication_rel_oid - 1] = ObjectIdGetDatum(pubreloid);
	values[Anum_pg_publication_rel_prpubid - 1] =
		ObjectIdGetDatum(pubid);
	values[Anum_pg_publication_rel_prrelid - 1] =
		ObjectIdGetDatum(relid);
	values[Anum_pg_publication_rel_prexcept - 1] =
		BoolGetDatum(pri->except);

	/* Add qualifications, if available */
	if (pri->whereClause != NULL)
		values[Anum_pg_publication_rel_prqual - 1] = CStringGetTextDatum(nodeToString(pri->whereClause));
	else
		nulls[Anum_pg_publication_rel_prqual - 1] = true;

	/* Add column list, if available */
	if (pri->columns)
		values[Anum_pg_publication_rel_prattrs - 1] = PointerGetDatum(attnumstoint2vector(attnums));
	else
		nulls[Anum_pg_publication_rel_prattrs - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* Insert tuple into catalog. */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	/* Register dependencies as needed */
	ObjectAddressSet(myself, PublicationRelRelationId, pubreloid);

	/* Add dependency on the publication */
	ObjectAddressSet(referenced, PublicationRelationId, pubid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* Add dependency on the relation */
	ObjectAddressSet(referenced, RelationRelationId, relid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* Add dependency on the objects mentioned in the qualifications */
	if (pri->whereClause)
		recordDependencyOnSingleRelExpr(&myself, pri->whereClause, relid,
										DEPENDENCY_NORMAL, DEPENDENCY_NORMAL,
										false);

	/* Add dependency on the columns, if any are listed */
	i = -1;
	while ((i = bms_next_member(attnums, i)) >= 0)
	{
		ObjectAddressSubSet(referenced, RelationRelationId, relid, i);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Close the table. */
	table_close(rel, RowExclusiveLock);

	/*
	 * No need to invalidate the tables listed in the EXCEPT clause, because
	 * in that case all tables are invalidated anyway.
	 */
	if (!pri->except)
	{
		/*
		 * Invalidate relcache so that publication info is rebuilt.
		 *
		 * For the partitioned tables, we must invalidate all partitions
		 * contained in the respective partition hierarchies, not just the one
		 * explicitly mentioned in the publication. This is required because
		 * we implicitly publish the child tables when the parent table is
		 * published.
		 */
		relids = GetPubPartitionOptionRelations(relids, PUBLICATION_PART_ALL,
												relid);

		InvalidatePublicationRels(relids);
	}

	return myself;
}

/*
 * pub_collist_validate
 *		Process and validate the 'columns' list and ensure the columns are all
 *		valid to use for a publication.  Checks for and raises an ERROR for
 *		any unknown columns, system columns, duplicate columns, or virtual
 *		generated columns.
 *
 * Looks up each column's attnum and returns a 0-based Bitmapset of the
 * corresponding attnums.
 */
Bitmapset *
pub_collist_validate(Relation targetrel, List *columns)
{
	Bitmapset  *set = NULL;
	ListCell   *lc;
	TupleDesc	tupdesc = RelationGetDescr(targetrel);

	foreach(lc, columns)
	{
		char	   *colname = strVal(lfirst(lc));
		AttrNumber	attnum = get_attnum(RelationGetRelid(targetrel), colname);

		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" of relation \"%s\" does not exist",
						   colname, RelationGetRelationName(targetrel)));

		if (!AttrNumberIsForUserDefinedAttr(attnum))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					errmsg("cannot use system column \"%s\" in publication column list",
						   colname));

		if (TupleDescAttr(tupdesc, attnum - 1)->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					errmsg("cannot use virtual generated column \"%s\" in publication column list",
						   colname));

		if (bms_is_member(attnum, set))
			ereport(ERROR,
					errcode(ERRCODE_DUPLICATE_OBJECT),
					errmsg("duplicate column \"%s\" in publication column list",
						   colname));

		set = bms_add_member(set, attnum);
	}

	return set;
}

/*
 * Transform a column list (represented by an array Datum) to a bitmapset.
 *
 * If columns isn't NULL, add the column numbers to that set.
 *
 * If mcxt isn't NULL, build the bitmapset in that context.
 */
Bitmapset *
pub_collist_to_bitmapset(Bitmapset *columns, Datum pubcols, MemoryContext mcxt)
{
	Bitmapset  *result = columns;
	ArrayType  *arr;
	int			nelems;
	int16	   *elems;
	MemoryContext oldcxt = NULL;

	arr = DatumGetArrayTypeP(pubcols);
	nelems = ARR_DIMS(arr)[0];
	elems = (int16 *) ARR_DATA_PTR(arr);

	/* If a memory context was specified, switch to it. */
	if (mcxt)
		oldcxt = MemoryContextSwitchTo(mcxt);

	for (int i = 0; i < nelems; i++)
		result = bms_add_member(result, elems[i]);

	if (mcxt)
		MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * Returns a bitmap representing the columns of the specified table.
 *
 * Generated columns are included if include_gencols_type is
 * PUBLISH_GENCOLS_STORED.
 */
Bitmapset *
pub_form_cols_map(Relation relation, PublishGencolsType include_gencols_type)
{
	Bitmapset  *result = NULL;
	TupleDesc	desc = RelationGetDescr(relation);

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
			continue;

		if (att->attgenerated)
		{
			/* We only support replication of STORED generated cols. */
			if (att->attgenerated != ATTRIBUTE_GENERATED_STORED)
				continue;

			/* User hasn't requested to replicate STORED generated cols. */
			if (include_gencols_type != PUBLISH_GENCOLS_STORED)
				continue;
		}

		result = bms_add_member(result, att->attnum);
	}

	return result;
}

/*
 * Insert new publication / schema mapping.
 */
ObjectAddress
publication_add_schema(Oid pubid, Oid schemaid, bool if_not_exists)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_publication_namespace];
	bool		nulls[Natts_pg_publication_namespace];
	Oid			psschid;
	Publication *pub = GetPublication(pubid);
	List	   *schemaRels = NIL;
	ObjectAddress myself,
				referenced;

	rel = table_open(PublicationNamespaceRelationId, RowExclusiveLock);

	/*
	 * Check for duplicates. Note that this does not really prevent
	 * duplicates, it's here just to provide nicer error message in common
	 * case. The real protection is the unique key on the catalog.
	 */
	if (SearchSysCacheExists2(PUBLICATIONNAMESPACEMAP,
							  ObjectIdGetDatum(schemaid),
							  ObjectIdGetDatum(pubid)))
	{
		table_close(rel, RowExclusiveLock);

		if (if_not_exists)
			return InvalidObjectAddress;

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("schema \"%s\" is already member of publication \"%s\"",
						get_namespace_name(schemaid), pub->name)));
	}

	check_publication_add_schema(schemaid);

	/* Form a tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	psschid = GetNewOidWithIndex(rel, PublicationNamespaceObjectIndexId,
								 Anum_pg_publication_namespace_oid);
	values[Anum_pg_publication_namespace_oid - 1] = ObjectIdGetDatum(psschid);
	values[Anum_pg_publication_namespace_pnpubid - 1] =
		ObjectIdGetDatum(pubid);
	values[Anum_pg_publication_namespace_pnnspid - 1] =
		ObjectIdGetDatum(schemaid);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* Insert tuple into catalog */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	ObjectAddressSet(myself, PublicationNamespaceRelationId, psschid);

	/* Add dependency on the publication */
	ObjectAddressSet(referenced, PublicationRelationId, pubid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* Add dependency on the schema */
	ObjectAddressSet(referenced, NamespaceRelationId, schemaid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* Close the table */
	table_close(rel, RowExclusiveLock);

	/*
	 * Invalidate relcache so that publication info is rebuilt. See
	 * publication_add_relation for why we need to consider all the
	 * partitions.
	 */
	schemaRels = GetSchemaPublicationRelations(schemaid,
											   PUBLICATION_PART_ALL);
	InvalidatePublicationRels(schemaRels);

	return myself;
}

/*
 * Get the list of publication oids associated with a specified relation.
 *
 * 'pubids' returns the Oids of the publications the relation is part of.
 *
 * 'except_pubids' returns the Oids of publications the relation is excluded
 * from.
 *
 * This function returns true if the relation is part of any publication.
 */
bool
GetRelationPublications(Oid relid, List **pubids, List **except_pubids)
{
	CatCList   *pubrellist;
	bool		found = false;

	/* Find all publications associated with the relation. */
	pubrellist = SearchSysCacheList1(PUBLICATIONRELMAP,
									 ObjectIdGetDatum(relid));
	for (int i = 0; i < pubrellist->n_members; i++)
	{
		HeapTuple	tup = &pubrellist->members[i]->tuple;
		Form_pg_publication_rel pubrel = (Form_pg_publication_rel) GETSTRUCT(tup);
		Oid			pubid = pubrel->prpubid;
		List	  **target_list = pubrel->prexcept ? except_pubids : pubids;

		if (target_list)
			*target_list = lappend_oid(*target_list, pubid);

		if (!pubrel->prexcept)
			found = true;
	}

	ReleaseSysCacheList(pubrellist);

	return found;
}

/*
 * Internal function to get the list of relation Oids for a publication.
 *
 * If except_flag is true, returns the list of relations specified in the
 * EXCEPT clause of the publication; otherwise, returns the list of relations
 * included in the publication.
 */
static List *
get_publication_relations(Oid pubid, PublicationPartOpt pub_partopt,
						  bool except_flag)
{
	List	   *result;
	Relation	pubrelsrel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;

	/* Find all relations associated with the publication. */
	pubrelsrel = table_open(PublicationRelRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_publication_rel_prpubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(pubid));

	scan = systable_beginscan(pubrelsrel, PublicationRelPrpubidIndexId,
							  true, NULL, 1, &scankey);

	result = NIL;
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_publication_rel pubrel;

		pubrel = (Form_pg_publication_rel) GETSTRUCT(tup);

		if (except_flag == pubrel->prexcept)
			result = GetPubPartitionOptionRelations(result, pub_partopt,
													pubrel->prrelid);
	}

	systable_endscan(scan);
	table_close(pubrelsrel, AccessShareLock);

	/* Now sort and de-duplicate the result list */
	list_sort(result, list_oid_cmp);
	list_deduplicate_oid(result);

	return result;
}

/*
 * Gets list of relation oids that are associated with a publication.
 *
 * This should only be used FOR TABLE publications, the FOR ALL TABLES/SEQUENCES
 * should use GetAllPublicationRelations().
 */
List *
GetIncludedPublicationRelations(Oid pubid, PublicationPartOpt pub_partopt)
{
	Assert(!GetPublication(pubid)->alltables);

	return get_publication_relations(pubid, pub_partopt, false);
}

/*
 * Gets list of table oids that were specified in the except clause for a
 * publication.
 *
 * This should only be used FOR TABLE publications, the FOR ALL TABLES/SEQUENCES
 * should use GetAllPublicationRelations().
 */
List *
GetExcludedPublicationTables(Oid pubid, PublicationPartOpt pub_partopt)
{
	Assert(GetPublication(pubid)->alltables);

	return get_publication_relations(pubid, pub_partopt, true);
}

/*
 * Gets list of publication oids for publications marked as FOR ALL TABLES.
 */
List *
GetAllTablesPublications(void)
{
	List	   *result;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;

	/* Find all publications that are marked as for all tables. */
	rel = table_open(PublicationRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_publication_puballtables,
				BTEqualStrategyNumber, F_BOOLEQ,
				BoolGetDatum(true));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, 1, &scankey);

	result = NIL;
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Oid			oid = ((Form_pg_publication) GETSTRUCT(tup))->oid;

		result = lappend_oid(result, oid);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * Returns true if the given partitioned table is effectively excluded
 * by the EXCEPT list.
 *
 * A relation is considered excluded if:
 *  1) It is explicitly present in exceptlist, or
 *  2) All of its leaf partitions are present in exceptlist.
 */
static bool
relation_is_effectively_excluded(Oid relid, List *exceptlist)
{
	List	   *leaftables = NIL;

	if (exceptlist == NIL)
		return false;

	/* Explicitly excluded */
	if (list_member_oid(exceptlist, relid))
		return true;

	/* Get all leaf partitions of relid */
	leaftables = GetPubPartitionOptionRelations(leaftables,
												PUBLICATION_PART_LEAF,
												relid);

	/*
	 * If any leaf is NOT present in exceptlist, then the relation is not
	 * fully excluded.
	 */
	foreach_oid(leafrelid, leaftables)
	{
		if (!list_member_oid(exceptlist, leafrelid))
			return false;
	}

	/* All leaves are excluded */
	return true;
}

/*
 * Gets list of all relations published by FOR ALL TABLES/SEQUENCES
 * publication.
 *
 * If the publication publishes partition changes via their respective root
 * partitioned tables, we must exclude partitions in favor of including the
 * root partitioned tables. This is not applicable to FOR ALL SEQUENCES
 * publication.
 *
 * For a FOR ALL TABLES publication, the returned list excludes tables mentioned
 * in EXCEPT TABLE clause.
 */
List *
GetAllPublicationRelations(Publication *pub, char relkind)
{
	Relation	classRel;
	ScanKeyData key[1];
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;
	List	   *exceptlist = NIL;
	bool		pubviaroot = pub->pubviaroot;
	Oid			pubid = pub->oid;

	Assert(!(relkind == RELKIND_SEQUENCE && pubviaroot));

	if (relkind == RELKIND_RELATION)
		exceptlist = GetExcludedPublicationTables(pubid, pubviaroot ?
												  PUBLICATION_PART_ALL :
												  PUBLICATION_PART_LEAF);

	classRel = table_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(relkind));

	scan = table_beginscan_catalog(classRel, 1, key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class relForm = (Form_pg_class) GETSTRUCT(tuple);
		Oid			relid = relForm->oid;

		if (is_publishable_class(relid, relForm) &&
			!(relForm->relispartition && pubviaroot) &&
			!list_member_oid(exceptlist, relid))
			result = lappend_oid(result, relid);
	}

	table_endscan(scan);

	if (pubviaroot)
	{
		ScanKeyInit(&key[0],
					Anum_pg_class_relkind,
					BTEqualStrategyNumber, F_CHAREQ,
					CharGetDatum(RELKIND_PARTITIONED_TABLE));

		scan = table_beginscan_catalog(classRel, 1, key);

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			Form_pg_class relForm = (Form_pg_class) GETSTRUCT(tuple);
			Oid			relid = relForm->oid;

			if (is_publishable_class(relid, relForm) &&
				!relForm->relispartition &&
				!relation_is_effectively_excluded(relid, exceptlist))
				result = lappend_oid(result, relid);
		}

		table_endscan(scan);
	}

	table_close(classRel, AccessShareLock);
	return result;
}

/*
 * Gets the list of schema oids for a publication.
 *
 * This should only be used FOR TABLES IN SCHEMA publications.
 */
List *
GetPublicationSchemas(Oid pubid)
{
	List	   *result = NIL;
	Relation	pubschsrel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;

	/* Find all schemas associated with the publication */
	pubschsrel = table_open(PublicationNamespaceRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_publication_namespace_pnpubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(pubid));

	scan = systable_beginscan(pubschsrel,
							  PublicationNamespacePnnspidPnpubidIndexId,
							  true, NULL, 1, &scankey);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_publication_namespace pubsch;

		pubsch = (Form_pg_publication_namespace) GETSTRUCT(tup);

		result = lappend_oid(result, pubsch->pnnspid);
	}

	systable_endscan(scan);
	table_close(pubschsrel, AccessShareLock);

	return result;
}

/*
 * Gets the list of publication oids associated with a specified schema.
 */
List *
GetSchemaPublications(Oid schemaid)
{
	List	   *result = NIL;
	CatCList   *pubschlist;
	int			i;

	/* Find all publications associated with the schema */
	pubschlist = SearchSysCacheList1(PUBLICATIONNAMESPACEMAP,
									 ObjectIdGetDatum(schemaid));
	for (i = 0; i < pubschlist->n_members; i++)
	{
		HeapTuple	tup = &pubschlist->members[i]->tuple;
		Oid			pubid = ((Form_pg_publication_namespace) GETSTRUCT(tup))->pnpubid;

		result = lappend_oid(result, pubid);
	}

	ReleaseSysCacheList(pubschlist);

	return result;
}

/*
 * Get the list of publishable relation oids for a specified schema.
 */
List *
GetSchemaPublicationRelations(Oid schemaid, PublicationPartOpt pub_partopt)
{
	Relation	classRel;
	ScanKeyData key[1];
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	Assert(OidIsValid(schemaid));

	classRel = table_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_relnamespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(schemaid));

	/* get all the relations present in the specified schema */
	scan = table_beginscan_catalog(classRel, 1, key);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class relForm = (Form_pg_class) GETSTRUCT(tuple);
		Oid			relid = relForm->oid;
		char		relkind;

		if (!is_publishable_class(relid, relForm))
			continue;

		relkind = get_rel_relkind(relid);
		if (relkind == RELKIND_RELATION)
			result = lappend_oid(result, relid);
		else if (relkind == RELKIND_PARTITIONED_TABLE)
		{
			List	   *partitionrels = NIL;

			/*
			 * It is quite possible that some of the partitions are in a
			 * different schema than the parent table, so we need to get such
			 * partitions separately.
			 */
			partitionrels = GetPubPartitionOptionRelations(partitionrels,
														   pub_partopt,
														   relForm->oid);
			result = list_concat_unique_oid(result, partitionrels);
		}
	}

	table_endscan(scan);
	table_close(classRel, AccessShareLock);
	return result;
}

/*
 * Gets the list of all relations published by FOR TABLES IN SCHEMA
 * publication.
 */
List *
GetAllSchemaPublicationRelations(Oid pubid, PublicationPartOpt pub_partopt)
{
	List	   *result = NIL;
	List	   *pubschemalist = GetPublicationSchemas(pubid);
	ListCell   *cell;

	foreach(cell, pubschemalist)
	{
		Oid			schemaid = lfirst_oid(cell);
		List	   *schemaRels = NIL;

		schemaRels = GetSchemaPublicationRelations(schemaid, pub_partopt);
		result = list_concat(result, schemaRels);
	}

	return result;
}

/*
 * Get publication using oid
 *
 * The Publication struct and its data are palloc'ed here.
 */
Publication *
GetPublication(Oid pubid)
{
	HeapTuple	tup;
	Publication *pub;
	Form_pg_publication pubform;

	tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication %u", pubid);

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	pub = palloc_object(Publication);
	pub->oid = pubid;
	pub->name = pstrdup(NameStr(pubform->pubname));
	pub->alltables = pubform->puballtables;
	pub->allsequences = pubform->puballsequences;
	pub->pubactions.pubinsert = pubform->pubinsert;
	pub->pubactions.pubupdate = pubform->pubupdate;
	pub->pubactions.pubdelete = pubform->pubdelete;
	pub->pubactions.pubtruncate = pubform->pubtruncate;
	pub->pubviaroot = pubform->pubviaroot;
	pub->pubgencols_type = pubform->pubgencols;

	ReleaseSysCache(tup);

	return pub;
}

/*
 * Get Publication using name.
 */
Publication *
GetPublicationByName(const char *pubname, bool missing_ok)
{
	Oid			oid;

	oid = get_publication_oid(pubname, missing_ok);

	return OidIsValid(oid) ? GetPublication(oid) : NULL;
}

/*
 * publication_has_any_except_table
 *
 * Returns true if the given publication OID has at least one entry in
 * pg_publication_rel marked as an exception (prexcept = true).
 */
bool
publication_has_any_except_table(Oid pubid)
{
	Relation	pubrelsrel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;
	bool		found = false;

	pubrelsrel = table_open(PublicationRelRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_publication_rel_prpubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(pubid));

	scan = systable_beginscan(pubrelsrel,
							  PublicationRelPrpubidIndexId,
							  true, NULL, 1, &scankey);

	/* We only need to find any occurrence of prexcept = true */
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_publication_rel pubrel;

		pubrel = (Form_pg_publication_rel) GETSTRUCT(tup);
		if (pubrel->prexcept)
		{
			found = true;
			break;
		}
	}

	systable_endscan(scan);
	table_close(pubrelsrel, AccessShareLock);

	return found;
}

/* is_relid_excepted
 *
 * Check if the relation 'relid' is explicitly specified in the EXCEPT clause
 * of the given publication.
 */
bool
is_relid_excepted(Oid relid, Oid pubid)
{
	HeapTuple	tup;
	bool		result = false;

	tup = SearchSysCache2(PUBLICATIONRELMAP, ObjectIdGetDatum(relid),
						  ObjectIdGetDatum(pubid));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_publication_rel prform;
		
		prform = (Form_pg_publication_rel) GETSTRUCT(tup);
		result = prform->prexcept;

		ReleaseSysCache(tup);
	}
	return result;
}

/*
 * is_relid_or_ancestor_excepted
 *
 * Check if the relation 'relid' or any of its partition ancestors are
 * explicitly specified in the EXCEPT clause of the given publication.
 */
static bool
is_relid_or_ancestor_excepted(Oid relid, Oid pubid)
{
	List	   *ancestors;
	ListCell   *lc;
	bool		in_except = false;

	/* Check the relation itself first */
	if (is_relid_excepted(relid, pubid))
		return true;

	/* Check the inheritance chain */
	ancestors = get_partition_ancestors(relid);

	foreach(lc, ancestors)
	{
		Oid			ancestor = lfirst_oid(lc);

		if (is_relid_excepted(ancestor, pubid))
		{
			in_except = true;
			break;
		}
	}

	list_free(ancestors);

	return in_except;
}

/*
 * is_relid_published
 *
 * Check whether a given table or its schema is included in the specified
 * publication.
 */
static bool
is_relid_published(Oid relid, Oid pubid)
{
	HeapTuple	tup;

	tup = SearchSysCache2(PUBLICATIONRELMAP, ObjectIdGetDatum(relid),
						  ObjectIdGetDatum(pubid));
	if (HeapTupleIsValid(tup))
	{
		bool		published = false;
		Form_pg_publication_rel prform;

		prform = (Form_pg_publication_rel) GETSTRUCT(tup);
		published = !prform->prexcept;

		ReleaseSysCache(tup);

		if (published)
			return true;
	}

	return SearchSysCacheExists2(PUBLICATIONNAMESPACEMAP,
								 ObjectIdGetDatum(get_rel_namespace(relid)),
								 ObjectIdGetDatum(pubid));
}

/*
 * is_relid_or_ancestor_published
 *
 * Check whether a given table or its schema or any of its partition ancestors,
 * or its schema included in the specified publication
 */
static bool
is_relid_or_ancestor_published(Oid relid, Oid pubid)
{
	if (is_relid_published(relid, pubid))
		return true;
	else
	{
		List	   *ancestors = get_partition_ancestors(relid);

		foreach_oid(anc_oid, ancestors)
		{
			if (is_relid_published(anc_oid, pubid))
				return true;
		}
	}

	return false;
}

/*
 * pg_get_publication_effective_tables
 *
 * Given a root partitioned table and a list of publications, calculate the set
 * of relations that are effectively published. This is necessary for
 * "FOR ALL TABLES" publications that use "EXCEPT TABLE" filters.
 *
 * The function returns a minimal set of relations that collectively
 * include all non-excluded leaf partitions in the partition hierarchy.
 */
Datum
pg_get_publication_effective_tables(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *results;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			root_relid = PG_GETARG_OID(0);
		ArrayType  *pub_names_array = PG_GETARG_ARRAYTYPE_P(1);
		MemoryContext oldcontext;
		List	   *pub_oids = NIL;
		Datum	   *pub_datums;
		bool	   *pub_nulls;
		int			pub_count;
		TupleDesc	tupdesc;
		List	   *final_output = NIL;
		bool		has_clean_all_tables_pub = false;
		List	   *except_pub_names = NIL;
		Oid			except_pub_id = InvalidOid;

		Assert(get_rel_relkind(root_relid) == RELKIND_PARTITIONED_TABLE);

		funcctx = SRF_FIRSTCALL_INIT();

		deconstruct_array(pub_names_array, TEXTOID, -1, false, 'i',
						  &pub_datums, &pub_nulls, &pub_count);

		/* Build the list of pub_oids */
		for (int i = 0; i < pub_count; i++)
		{
			if (!pub_nulls[i])
			{
				char	   *pubname = TextDatumGetCString(pub_datums[i]);

				pub_oids = lappend_oid(pub_oids, get_publication_oid(pubname, false));
			}
		}

		/*
		 * Determine whether the expensive expansion step can be skipped. If
		 * any publication is a FOR ALL TABLES publication without an EXCEPT
		 * clause, the root relation alone is sufficient as the result.
		 */
		foreach_oid(puboid, pub_oids)
		{
			HeapTuple	pubTup;
			Form_pg_publication pubform;

			pubTup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(puboid));
			if (!HeapTupleIsValid(pubTup))
				continue;

			pubform = (Form_pg_publication) GETSTRUCT(pubTup);
			if (pubform->puballtables)
			{
				/* Check whether this publication defines any EXCEPT entries */
				if (publication_has_any_except_table(puboid))
				{
					except_pub_names = lappend(except_pub_names,
											   makeString(pubform->pubname.data));
					except_pub_id = pubform->oid;
				}
				else
				{
					/* This publication includes all tables without except */
					has_clean_all_tables_pub = true;
				}
			}

			ReleaseSysCache(pubTup);
		}

		if (list_length(except_pub_names) > 1)
		{
			StringInfo	pub_names = makeStringInfo();

			GetPublicationsStr(except_pub_names, pub_names, true);
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot combine publications that define EXCEPT TABLE clauses"),
					errdetail("The following publications define EXCEPT TABLE clauses: %s.",
							  pub_names->data));
		}

		/* Return root immediately if no filtering logic is needed */
		if (has_clean_all_tables_pub || !OidIsValid(except_pub_id))
		{
			oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
			final_output = list_make1_oid(root_relid);
			MemoryContextSwitchTo(oldcontext);
		}

		/*
		 * Build the effective publication relation list for a partitioned
		 * hierarchy in the presence of an EXCEPT publication.
		 *
		 * 1. Collect all leaf partitions under the given root relation.
		 *
		 * 2. Classify each leaf as either: - excepted_leaves: explicitly
		 * excluded via the EXCEPT clause (either directly or through any of
		 * its ancestors), or - allowed_leaves: not excluded and therefore
		 * part of the effective publication set.
		 *
		 * 3. Re-evaluate excluded leaves against other publications. A leaf
		 * excluded in one publication may still be effectively included if it
		 * (or any of its ancestors, or its schema) is published through
		 * another publication. Such leaves are added back to allowed_leaves.
		 *
		 * 4. Bottom-up collapse of partition branches. For each partitioned
		 * table in the hierarchy: - If *all* of its leaf partitions are
		 * present in allowed_leaves, the parent can represent the entire
		 * branch. - Such parents are added as candidates, allowing
		 * higher-level representation instead of listing every leaf
		 * individually.
		 *
		 * 5. Deduplicate and normalize the result. Remove any relation whose
		 * ancestor is already selected as a candidate. This ensures the final
		 * output contains only the highest-level representative for each
		 * fully-allowed branch and avoids redundant entries.
		 *
		 * The final_output therefore contains a minimal, non-redundant set of
		 * relations that accurately represents the effective publication set
		 * after considering EXCEPT rules and multiple publications.
		 */
		else
		{
			List	   *all_tables;
			List	   *all_leaves = NIL;
			List	   *excepted_leaves = NIL;
			List	   *allowed_leaves = NIL;
			List	   *candidate_list = NIL;

			/* Get all the leaf relations */
			all_leaves = GetPubPartitionOptionRelations(all_leaves,
														PUBLICATION_PART_LEAF,
														root_relid);
			foreach_oid(curr_relid, all_leaves)
			{
				/*
				 * A leaf table is considered excluded if it, or any of its
				 * ancestors, is listed in the EXCEPT clause of the
				 * publication. Otherwise, it remains part of the effective
				 * publication set.
				 */
				if (is_relid_or_ancestor_excepted(curr_relid, except_pub_id))
					excepted_leaves = lappend_oid(excepted_leaves, curr_relid);
				else
					allowed_leaves = lappend_oid(allowed_leaves, curr_relid);
			}

			/*
			 * A table excluded by the EXCEPT clause of one publication may
			 * still be included if it is explicitly published, or published
			 * via its schema or any of its ancestors, in another publication.
			 */
			foreach_oid(curr_relid, excepted_leaves)
			{
				foreach_oid(pubid, pub_oids)
				{
					/* Skip the publication that excluded this relation. */
					if (pubid == except_pub_id)
						continue;

					if (is_relid_or_ancestor_published(curr_relid, pubid))
						allowed_leaves = lappend_oid(allowed_leaves,
													 curr_relid);
				}
			}

			/* Bottom-Up Collapse. Check if parents can represent children */
			all_tables = find_all_inheritors(root_relid, AccessShareLock, NULL);
			candidate_list = list_copy(allowed_leaves);
			foreach_oid(curr_relid, all_tables)
			{
				List	   *branch_leaves = NIL;
				bool		all_allowed = true;

				/* Only consider partitioned tables as collapse candidates */
				if (get_rel_relkind(curr_relid) != RELKIND_PARTITIONED_TABLE)
					continue;

				branch_leaves = GetPubPartitionOptionRelations(branch_leaves,
															   PUBLICATION_PART_LEAF,
															   curr_relid);
				if (branch_leaves == NIL)
					continue;

				foreach_oid(lcb_oid, branch_leaves)
				{
					if (!list_member_oid(allowed_leaves, lcb_oid))
					{
						all_allowed = false;
						break;
					}
				}

				if (all_allowed)
					candidate_list = list_append_unique_oid(candidate_list,
															curr_relid);
			}

			/*
			 * Deduplicate: Filter out any relation whose ancestor is already
			 * present in the candidate list. This ensures we only return the
			 * "highest" representative for each branch.
			 */
			oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

			foreach_oid(curr_relid, candidate_list)
			{
				List	   *ancestors = get_partition_ancestors(curr_relid);
				bool		ancestor_already_included = false;

				/*
				 * Check if any ancestor of the current relation exists in the
				 * candidate list. If so, this relation is redundant.
				 */
				foreach_oid(ancestor_relid, ancestors)
				{
					if (list_member_oid(candidate_list, ancestor_relid))
					{
						ancestor_already_included = true;
						break;
					}
				}

				if (!ancestor_already_included)
					final_output = lappend_oid(final_output, curr_relid);

				list_free(ancestors);
			}

			MemoryContextSwitchTo(oldcontext);
		}

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		/* Construct a tuple descriptor for the result rows. */
		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "nspname",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "relname",
						   TEXTOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = final_output;

		MemoryContextSwitchTo(oldcontext);
	}

	/* SRF Per-call Resume */
	funcctx = SRF_PERCALL_SETUP();
	results = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < list_length(results))
	{
		Oid			current_relid = list_nth_oid(results, (int) funcctx->call_cntr);
		HeapTuple	rettuple;
		Datum		values[2];
		bool		nulls[2] = {false, false};

		values[0] = CStringGetTextDatum(get_namespace_name(get_rel_namespace(current_relid)));
		values[1] = CStringGetTextDatum(get_rel_name(current_relid));

		rettuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(rettuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Get information of the tables in the given publication array.
 *
 * Returns pubid, relid, column list, row filter for each table.
 */
Datum
pg_get_publication_tables(PG_FUNCTION_ARGS)
{
#define NUM_PUBLICATION_TABLES_ELEM	4
	FuncCallContext *funcctx;
	List	   *table_infos = NIL;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;
		ArrayType  *arr;
		Datum	   *elems;
		int			nelems,
					i;
		bool		viaroot = false;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * Deconstruct the parameter into elements where each element is a
		 * publication name.
		 */
		arr = PG_GETARG_ARRAYTYPE_P(0);
		deconstruct_array_builtin(arr, TEXTOID, &elems, NULL, &nelems);

		/* Get Oids of tables from each publication. */
		for (i = 0; i < nelems; i++)
		{
			Publication *pub_elem;
			List	   *pub_elem_tables = NIL;
			ListCell   *lc;

			pub_elem = GetPublicationByName(TextDatumGetCString(elems[i]), false);

			/*
			 * Publications support partitioned tables. If
			 * publish_via_partition_root is false, all changes are replicated
			 * using leaf partition identity and schema, so we only need
			 * those. Otherwise, get the partitioned table itself.
			 */
			if (pub_elem->alltables)
				pub_elem_tables = GetAllPublicationRelations(pub_elem,
															 RELKIND_RELATION);
			else
			{
				List	   *relids,
						   *schemarelids;

				relids = GetIncludedPublicationRelations(pub_elem->oid,
														 pub_elem->pubviaroot ?
														 PUBLICATION_PART_ROOT :
														 PUBLICATION_PART_LEAF);
				schemarelids = GetAllSchemaPublicationRelations(pub_elem->oid,
																pub_elem->pubviaroot ?
																PUBLICATION_PART_ROOT :
																PUBLICATION_PART_LEAF);
				pub_elem_tables = list_concat_unique_oid(relids, schemarelids);
			}

			/*
			 * Record the published table and the corresponding publication so
			 * that we can get row filters and column lists later.
			 *
			 * When a table is published by multiple publications, to obtain
			 * all row filters and column lists, the structure related to this
			 * table will be recorded multiple times.
			 */
			foreach(lc, pub_elem_tables)
			{
				published_rel *table_info = palloc_object(published_rel);

				table_info->relid = lfirst_oid(lc);
				table_info->pubid = pub_elem->oid;
				table_infos = lappend(table_infos, table_info);
			}

			/* At least one publication is using publish_via_partition_root. */
			if (pub_elem->pubviaroot)
				viaroot = true;
		}

		/*
		 * If the publication publishes partition changes via their respective
		 * root partitioned tables, we must exclude partitions in favor of
		 * including the root partitioned tables. Otherwise, the function
		 * could return both the child and parent tables which could cause
		 * data of the child table to be double-published on the subscriber
		 * side.
		 */
		if (viaroot)
			filter_partitions(table_infos);

		/* Construct a tuple descriptor for the result rows. */
		tupdesc = CreateTemplateTupleDesc(NUM_PUBLICATION_TABLES_ELEM);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "pubid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "relid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "attrs",
						   INT2VECTOROID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "qual",
						   PG_NODE_TREEOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = table_infos;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	table_infos = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < list_length(table_infos))
	{
		HeapTuple	pubtuple = NULL;
		HeapTuple	rettuple;
		Publication *pub;
		published_rel *table_info = (published_rel *) list_nth(table_infos, funcctx->call_cntr);
		Oid			relid = table_info->relid;
		Oid			schemaid = get_rel_namespace(relid);
		Datum		values[NUM_PUBLICATION_TABLES_ELEM] = {0};
		bool		nulls[NUM_PUBLICATION_TABLES_ELEM] = {0};

		/*
		 * Form tuple with appropriate data.
		 */

		pub = GetPublication(table_info->pubid);

		values[0] = ObjectIdGetDatum(pub->oid);
		values[1] = ObjectIdGetDatum(relid);

		/*
		 * We don't consider row filters or column lists for FOR ALL TABLES or
		 * FOR TABLES IN SCHEMA publications.
		 */
		if (!pub->alltables &&
			!SearchSysCacheExists2(PUBLICATIONNAMESPACEMAP,
								   ObjectIdGetDatum(schemaid),
								   ObjectIdGetDatum(pub->oid)))
			pubtuple = SearchSysCacheCopy2(PUBLICATIONRELMAP,
										   ObjectIdGetDatum(relid),
										   ObjectIdGetDatum(pub->oid));

		if (HeapTupleIsValid(pubtuple))
		{
			/* Lookup the column list attribute. */
			values[2] = SysCacheGetAttr(PUBLICATIONRELMAP, pubtuple,
										Anum_pg_publication_rel_prattrs,
										&(nulls[2]));

			/* Null indicates no filter. */
			values[3] = SysCacheGetAttr(PUBLICATIONRELMAP, pubtuple,
										Anum_pg_publication_rel_prqual,
										&(nulls[3]));
		}
		else
		{
			nulls[2] = true;
			nulls[3] = true;
		}

		/* Show all columns when the column list is not specified. */
		if (nulls[2])
		{
			Relation	rel = table_open(relid, AccessShareLock);
			int			nattnums = 0;
			int16	   *attnums;
			TupleDesc	desc = RelationGetDescr(rel);
			int			i;

			attnums = palloc_array(int16, desc->natts);

			for (i = 0; i < desc->natts; i++)
			{
				Form_pg_attribute att = TupleDescAttr(desc, i);

				if (att->attisdropped)
					continue;

				if (att->attgenerated)
				{
					/* We only support replication of STORED generated cols. */
					if (att->attgenerated != ATTRIBUTE_GENERATED_STORED)
						continue;

					/*
					 * User hasn't requested to replicate STORED generated
					 * cols.
					 */
					if (pub->pubgencols_type != PUBLISH_GENCOLS_STORED)
						continue;
				}

				attnums[nattnums++] = att->attnum;
			}

			if (nattnums > 0)
			{
				values[2] = PointerGetDatum(buildint2vector(attnums, nattnums));
				nulls[2] = false;
			}

			table_close(rel, AccessShareLock);
		}

		rettuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(rettuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Returns Oids of sequences in a publication.
 */
Datum
pg_get_publication_sequences(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *sequences = NIL;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		char	   *pubname = text_to_cstring(PG_GETARG_TEXT_PP(0));
		Publication *publication;
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		publication = GetPublicationByName(pubname, false);

		if (publication->allsequences)
			sequences = GetAllPublicationRelations(publication, RELKIND_SEQUENCE);

		funcctx->user_fctx = sequences;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	sequences = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < list_length(sequences))
	{
		Oid			relid = list_nth_oid(sequences, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, ObjectIdGetDatum(relid));
	}

	SRF_RETURN_DONE(funcctx);
}
