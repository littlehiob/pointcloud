/***********************************************************************
* pc_filter.c
*
*  Pointclound patch filtering.
*
*  Copyright (c) 2013 OpenGeo
*
***********************************************************************/

#include "pc_api_internal.h"
#include <assert.h>

PCBITMAP *
pc_bitmap_new(uint32_t npoints)
{
	PCBITMAP *map = pcalloc(sizeof(PCBITMAP));
	map->map = pcalloc(sizeof(uint8_t)*npoints);
	map->npoints = npoints;
	map->nset = 0;
	return map;
}

void
pc_bitmap_free(PCBITMAP *map)
{
	if (map->map) pcfree(map->map);
	pcfree(map);
}

inline void
pc_bitmap_set(PCBITMAP *map, int i, int val)
{
	uint8_t curval = map->map[i];
	if ( val && (!curval) )
		map->nset++;
	if ( (!val) && curval )
		map->nset--;

	map->map[i] = (val!=0);
}

inline uint8_t
pc_bitmap_get(const PCBITMAP *map, int i)
{
	return map->map[i];
}

void
pc_bitmap_filter(PCBITMAP *map, PC_FILTERTYPE filter, int i, double d, double val1, double val2)
{
	switch ( filter )
	{
	case PC_GT:
		pc_bitmap_set(map, i, (d > val1 ? 1 : 0));
		break;
	case PC_LT:
		pc_bitmap_set(map, i, (d < val1 ? 1 : 0));
		break;
	case PC_EQUAL:
		pc_bitmap_set(map, i, (d == val1 ? 1 : 0));
		break;
	case PC_BETWEEN:
		pc_bitmap_set(map, i, (d > val1 && d < val2 ? 1 : 0));
		break;
	}
}

static PCBITMAP *
pc_patch_uncompressed_bitmap(const PCPATCH_UNCOMPRESSED *pa, uint32_t dimnum, PC_FILTERTYPE filter, double val1, double val2)
{
	PCPOINT pt;
	uint32_t i = 0;
	uint8_t *buf = pa->data;
	double d;
	size_t sz = pa->schema->size;
	PCBITMAP *map = pc_bitmap_new(pa->npoints);

	pt.readonly = PC_TRUE;
	pt.schema = pa->schema;

	while ( i < pa->npoints )
	{
		pt.data = buf;
		pc_point_get_double(&pt, pa->schema->dims[dimnum], &d);
		/* Apply the filter to the bitmap */
		pc_bitmap_filter(map, filter, i, d, val1, val2);
		/* Advance data pointer and counter */
		buf += sz;
		i++;
	}

	return map;
}


static PCPATCH_UNCOMPRESSED *
pc_patch_uncompressed_filter(const PCPATCH_UNCOMPRESSED *pu, const PCBITMAP *map)
{
	int i = 0, j = 0;
	size_t sz = pu->schema->size;
	PCPATCH_UNCOMPRESSED *fpu = pc_patch_uncompressed_make(pu->schema, map->nset);
	uint8_t *buf = pu->data;
	uint8_t *fbuf = fpu->data;

	assert(map->npoints == pu->npoints);

	while ( i < pu->npoints )
	{
		if ( pc_bitmap_get(map, i) )
		{
			memcpy(fbuf, buf, sz);
			fbuf += sz;
		}
		buf += sz;
		i++;
	}

	fpu->maxpoints = fpu->npoints = map->nset;

	if ( PC_FAILURE == pc_patch_uncompressed_compute_extent(fpu) )
	{
		pcerror("%s: failed to compute patch extent", __func__);
		return NULL;
	}

	if ( PC_FAILURE == pc_patch_uncompressed_compute_stats(fpu) )
	{
		pcerror("%s: failed to compute patch stats", __func__);
		return NULL;
	}

	return fpu;
}



static PCBITMAP *
pc_patch_dimensional_bitmap(const PCPATCH_DIMENSIONAL *pdl, uint32_t dimnum, PC_FILTERTYPE filter, double val1, double val2)
{
	assert(dimnum < pdl->schema->ndims);
	double unscaled1 = pc_value_unscale_unoffset(val1, pdl->schema->dims[dimnum]);
	double unscaled2 = pc_value_unscale_unoffset(val2, pdl->schema->dims[dimnum]);

	return pc_bytes_bitmap(&(pdl->bytes[dimnum]), filter, unscaled1, unscaled2);
}

static PCPATCH_DIMENSIONAL *
pc_patch_dimensional_filter(const PCPATCH_DIMENSIONAL *pdl, const PCBITMAP *map)
{
	int i = 0;
	PCPATCH_DIMENSIONAL *fpdl = pc_patch_dimensional_clone(pdl);

	for ( i = 0; i < pdl->schema->ndims; i++ )
	{
		fpdl->bytes[i] = pc_bytes_filter(&(pdl->bytes[i]), map);
	}

	fpdl->npoints = map->nset;
	return fpdl;
}

PCPATCH *
pc_patch_filter(const PCPATCH *pa, uint32_t dimnum, PC_FILTERTYPE filter, double val1, double val2)
{
	if ( ! pa ) return NULL;
	PCPATCH *paout;

	switch ( pa->type )
	{
	case PC_NONE:
	{
		PCBITMAP *map = pc_patch_uncompressed_bitmap((PCPATCH_UNCOMPRESSED*)pa, dimnum, filter, val1, val2);
		PCPATCH_UNCOMPRESSED *pu;
		if ( map->nset == 0 )
		{
			pc_bitmap_free(map);
			return (PCPATCH*)pc_patch_uncompressed_make(pa->schema, 0);
		}
		pu = pc_patch_uncompressed_filter((PCPATCH_UNCOMPRESSED*)pa, map);
		pc_bitmap_free(map);
		/* pc_patch_uncompressed_filter computes stats and bounds, so we're ready to return here */
		/* TODO, it could/should compute bounds and stats while filtering the points */
		paout = (PCPATCH*)pu;
		break;
	}
	case PC_GHT:
	{
        PCPATCH_GHT *pgh = pc_patch_ght_filter((PCPATCH_GHT*)pa, dimnum, filter, val1, val2);
		/* pc_patch_ght_filter computes the bounds itself */
		/* TODO: add stats computation to pc_patch_ght_filter */
		/* pc_patch_ght_filter is just re-using the input stats, which is wrong */
        paout = (PCPATCH*)pgh;
		break;
	}
	case PC_DIMENSIONAL:
	{
		PCBITMAP *map = pc_patch_dimensional_bitmap((PCPATCH_DIMENSIONAL*)pa, dimnum, filter, val1, val2);
		PCPATCH_DIMENSIONAL *pdl;
		if ( map->nset == 0 )
		{
			pc_bitmap_free(map);
			return (PCPATCH*)pc_patch_uncompressed_make(pa->schema, 0);
		}
		pdl = pc_patch_dimensional_filter((PCPATCH_DIMENSIONAL*)pa, map);
		pc_bitmap_free(map);
		paout = (PCPATCH*)pdl;

		/* pc_patch_dimensional_filter does not compute computes stats or bounds, so we need to do it here */
		/* TODO: this is expensive, we should do this while we are traversing the bytes in the filter process */
    	if ( PC_FAILURE == pc_patch_compute_extent(paout) )
    		pcerror("%s: pc_patch_compute_extent failed", __func__);

    	if ( PC_FAILURE == pc_patch_compute_stats(paout) )
    		pcerror("%s: pc_patch_compute_stats failed", __func__);

		break;
	}
	default:
		pcerror("%s: failure", __func__);
	}

	return paout;
}

PCPATCH *
pc_patch_filter_lt_by_name(const PCPATCH *pa, const char *name, double val)
{
	/* Error out if we can't find the name */
	PCDIMENSION *d = pc_schema_get_dimension_by_name(pa->schema, name);
	if ( ! d ) return NULL;

	return pc_patch_filter(pa, d->position, PC_LT, val, val);
}

PCPATCH *
pc_patch_filter_gt_by_name(const PCPATCH *pa, const char *name, double val)
{
	/* Error out if we can't find the name */
	PCDIMENSION *d = pc_schema_get_dimension_by_name(pa->schema, name);
	if ( ! d ) return NULL;

	return pc_patch_filter(pa, d->position, PC_GT, val, val);
}

PCPATCH *
pc_patch_filter_equal_by_name(const PCPATCH *pa, const char *name, double val)
{
	/* Error out if we can't find the name */
	PCDIMENSION *d = pc_schema_get_dimension_by_name(pa->schema, name);
	if ( ! d ) return NULL;

	return pc_patch_filter(pa, d->position, PC_EQUAL, val, val);
}

PCPATCH *
pc_patch_filter_between_by_name(const PCPATCH *pa, const char *name, double val1, double val2)
{
	/* Ensure val1 < val2 always */
	if ( val1 > val2 )
	{
		double tmp = val1;
		val1 = val2;
		val2 = tmp;
	}
	/* Error out if we can't find the name */
	PCDIMENSION *d = pc_schema_get_dimension_by_name(pa->schema, name);
	if ( ! d ) return NULL;

	return pc_patch_filter(pa, d->position, PC_BETWEEN, val1, val2);
}


