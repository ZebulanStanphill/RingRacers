// Frozen from upstream's R_PointOnSide and R_PointInSubsector (without the UWP cache): the walk over node_t, which reads
// the partition line and children of each node from nodes[].
static INT32 UpstreamPointOnSide(fixed_t x, fixed_t y, node_t *node)
{
	if (!node->dx)
		return x <= node->x ? node->dy > 0 : node->dy < 0;

	if (!node->dy)
		return y <= node->y ? node->dx < 0 : node->dx > 0;

	x -= node->x;
	y -= node->y;

	// Try to quickly decide by looking at sign bits.
	// also use a mask to avoid branch prediction
	INT32 mask = (node->dy ^ node->dx ^ x ^ y) >> 31;
	return (mask & ((node->dy ^ x) < 0)) |  // (left is negative)
		(~mask & (FixedMul(y, node->dx>>FRACBITS) >= FixedMul(node->dy>>FRACBITS, x)));
}

static subsector_t *UpstreamPointInSubsector(fixed_t x, fixed_t y)
{
	size_t nodenum = numnodes-1;

	while (!(nodenum & NF_SUBSECTOR))
		nodenum = nodes[nodenum].children[UpstreamPointOnSide(x, y, nodes+nodenum)];

	return &subsectors[nodenum & ~NF_SUBSECTOR];
}
