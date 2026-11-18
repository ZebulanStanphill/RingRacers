// Frozen index sort from src/hardware/hw_batching.c at 36a023c196, with
// the loop and qsort call moved into a function so the harness can call them.
// [ZE]: Compares one field of two keys, falling through to the next field when equal.
#define COMPARE_KEY_FIELD(field) \
	if (key1->field != key2->field) return key1->field < key2->field ? -1 : 1

// [ZE]: Compares two sort keys, which replaces comparing whole polygon entries.
static int comparePolygonKeys(const void *p1, const void *p2)
{
	const PolygonSortKey *key1 = (const PolygonSortKey *)p1;
	const PolygonSortKey *key2 = (const PolygonSortKey *)p2;
	COMPARE_KEY_FIELD(shader);
	COMPARE_KEY_FIELD(texture);
	COMPARE_KEY_FIELD(brightmap);
	COMPARE_KEY_FIELD(polyFlags);
	COMPARE_KEY_FIELD(polyColor);
	COMPARE_KEY_FIELD(tintColor);
	COMPARE_KEY_FIELD(fadeColor);
	COMPARE_KEY_FIELD(lightLevel);
	COMPARE_KEY_FIELD(fadeStart);
	COMPARE_KEY_FIELD(fadeEnd);
	COMPARE_KEY_FIELD(directional);
	COMPARE_KEY_FIELD(lightTableId);
	COMPARE_KEY_FIELD(index);
	return 0;
}

// [ZE]: Sorts indices into polygonSortKeys rather than the keys themselves: qsort moves
// whole elements, and moving 64-byte keys cost more than the comparisons saved.
static int comparePolygonIndices(const void *p1, const void *p2)
{
	// [ZE]: Compare through the keys that the indices point to.
	return comparePolygonKeys(&polygonSortKeys[*(const UINT32 *)p1], &polygonSortKeys[*(const UINT32 *)p2]);
}

static void sortPolygonIndices(UINT32 *polygonIndexArray, int polygonArraySize)
{
	int i;
	for (i = 0; i < polygonArraySize; i++)
	{
		polygonIndexArray[i] = i;
	}
	// [ZE]: Sort the indices by their keys.
	qsort(polygonIndexArray, polygonArraySize, sizeof(UINT32), comparePolygonIndices);
}
// END
