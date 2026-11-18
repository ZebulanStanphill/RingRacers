// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Sean "Sryder" Ryder
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  k_pathfind.c
/// \brief A* Pathfinding algorithm implementation for SRB2 code base.

#include "k_pathfind.h"

#include "doomdef.h"
#include "z_zone.h"
#include "k_bheap.h"

static const size_t DEFAULT_NODEARRAY_CAPACITY = 8U;
static const size_t DEFAULT_OPENSET_CAPACITY   = 8U;
static const size_t DEFAULT_CLOSEDSET_CAPACITY = 8U;


/*--------------------------------------------------
	static UINT32 K_NodeGetFScore(const pathfindnode_t *const node)

		Gets the FScore of a node. The FScore is the GScore plus the HScore.

	Input Arguments:-
		node - The node to get the FScore of

	Return:-
		The FScore of the node.
--------------------------------------------------*/
static UINT32 K_NodeGetFScore(const pathfindnode_t *const node)
{
	UINT32 fscore = UINT32_MAX;

	I_Assert(node != NULL);

	fscore = node->gscore + node->hscore;

	return fscore;
}

/*--------------------------------------------------
	static void K_NodeUpdateHeapIndex(void *const node, const size_t newheapindex)

		A callback for the Openset Binary Heap to be able to update the heapindex of the pathfindnodes when they are
		moved.

	Input Arguments:-
		node         - The node that has been updated, should be a pointer to a pathfindnode_t
		newheapindex - The new heapindex of the node.

	Return:-
		None
--------------------------------------------------*/
static void K_NodeUpdateHeapIndex(void *const node, const size_t newheapindex)
{
	if (node == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "NULL node in K_NodeUpdateHeapIndex.\n");
	}
	else
	{
		pathfindnode_t *truenode = (pathfindnode_t*)node;
		truenode->heapindex = newheapindex;
	}
}

// [ZE]: Keep scratch storage across searches. Only the waypoint code calls A*, and its
// callbacks do not start another search. Setup capacities still track the original growth.
static struct
{
	// [ZE]: Keep closed flags by node index without changing pathfindnode_t.
	pathfindnode_t *nodes;
	UINT8 *closed;
	size_t nodescapacity;
	pathfindnode_t **closedset;
	size_t closedsetcapacity;
	size_t *lookup;
	size_t lookupcapacity;
} pathfindscratch;

// [ZE]: Allocate new node storage without freeing the old array. The caller fixes
// live pointers while both arrays still exist.
static void K_PathfindReserveNodes(size_t capacity)
{
	if (capacity > pathfindscratch.nodescapacity)
	{
		// [ZE]: Only live nodes will be copied into this array.
		pathfindscratch.nodes = Z_Malloc(capacity * sizeof(*pathfindscratch.nodes), PU_STATIC, NULL);
		pathfindscratch.closed = Z_Realloc(pathfindscratch.closed, capacity * sizeof(*pathfindscratch.closed), PU_STATIC, NULL);
		pathfindscratch.nodescapacity = capacity;
	} // [ZE]: The old nodes remain available to the caller for pointer fix-ups.
}

// [ZE]: Grow backing storage only when it cannot fit the current setup capacity.
static void K_PathfindReserveClosedSet(size_t capacity)
{
	if (capacity > pathfindscratch.closedsetcapacity)
	{
		pathfindscratch.closedset = Z_Realloc(pathfindscratch.closedset, capacity * sizeof(*pathfindscratch.closedset), PU_STATIC, NULL);
		pathfindscratch.closedsetcapacity = capacity;
	}
}

// [ZE]: Replace the reverse node scan with linear probing. A slot stores the
// node index plus one, leaving zero for empty. There are no deletions.
static size_t K_PathfindLookupSlot(const pathfindnode_t *nodes, const void *nodedata, size_t mask)
{
	uintptr_t hash = (uintptr_t)nodedata;
	size_t slot;

	// [ZE]: Mix higher address bits into the low bits before masking.
	hash ^= hash >> 16;
	hash *= UINT32_C(0x7feb352d);
	hash ^= hash >> 15;
	slot = (size_t)hash & mask;

	while (pathfindscratch.lookup[slot] != 0U
		&& nodes[pathfindscratch.lookup[slot] - 1U].nodedata != nodedata)
	{
		slot = (slot + 1U) & mask;
	}

	return slot;
}

// [ZE]: Clear the active table at the start of a search and reinsert live
// indices when its size changes. Keep the table at most half full.
static void K_PathfindResetLookup(size_t capacity, const pathfindnode_t *nodes, size_t count)
{
	size_t i;
	if (capacity > pathfindscratch.lookupcapacity)
	{
		Z_Free(pathfindscratch.lookup);
		pathfindscratch.lookup = Z_Malloc(capacity * sizeof(*pathfindscratch.lookup), PU_STATIC, NULL);
		pathfindscratch.lookupcapacity = capacity;
	}
	memset(pathfindscratch.lookup, 0, capacity * sizeof(*pathfindscratch.lookup));
	for (i = 0; i < count; ++i)
	{
		const size_t slot = K_PathfindLookupSlot(nodes, nodes[i].nodedata, capacity - 1U);
		// [ZE]: Keep the node's index even if its address has changed.
		pathfindscratch.lookup[slot] = i + 1U;
	}
}

/*--------------------------------------------------
	static boolean K_PathfindSetupValid(const pathfindsetup_t *const pathfindsetup)

		Checks that the setup given for pathfinding is valid and can be used.

	Input Arguments:-
		pathfindsetup - The setup for the pathfinding given

	Return:-
		True if pathfinding setup is valid, false if it isn't.
--------------------------------------------------*/
static boolean K_PathfindSetupValid(const pathfindsetup_t *const pathfindsetup)
{
	boolean pathfindsetupvalid = false;
	size_t sourcenodenumconnectednodes = 0U;
	size_t endnodenumconnectednodes    = 0U;

	if (pathfindsetup == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "NULL pathfindsetup in K_PathfindSetupValid.\n");
	}
	else if (pathfindsetup->startnodedata == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "Pathfindsetup has NULL startnodedata.\n");
	}
	else if (pathfindsetup->endnodedata == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "Pathfindsetup has NULL endnodedata.\n");
	}
	else if (pathfindsetup->getconnectednodes == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "Pathfindsetup has NULL getconnectednodes function.\n");
	}
	else if (pathfindsetup->getconnectioncosts == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "Pathfindsetup has NULL getconnectioncosts function.\n");
	}
	else if (pathfindsetup->getheuristic == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "Pathfindsetup has NULL getheuristic function.\n");
	}
	else if (pathfindsetup->gettraversable == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "Pathfindsetup has NULL gettraversable function.\n");
	}
	else if (pathfindsetup->getfinished == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "Pathfindsetup has NULL getfinished function.\n");
	}
	else if (pathfindsetup->getconnectednodes(pathfindsetup->startnodedata, &sourcenodenumconnectednodes) == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "K_PathfindSetupValid: Source node returned NULL connecting nodes.\n");
	}
	else if (sourcenodenumconnectednodes == 0U)
	{
		CONS_Debug(DBG_GAMELOGIC, "K_PathfindSetupValid: Source node has 0 connecting nodes.\n");
	}
	else if (pathfindsetup->getconnectednodes(pathfindsetup->endnodedata, &endnodenumconnectednodes) == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "K_PathfindSetupValid: End node returned NULL connecting nodes.\n");
	}
	else if (endnodenumconnectednodes == 0U)
	{
		CONS_Debug(DBG_GAMELOGIC, "K_PathfindSetupValid: End node has 0 connecting nodes.\n");
	}
	else
	{
		pathfindsetupvalid = true;
	}

	return pathfindsetupvalid;
}

static boolean K_ReconstructPath(path_t *const path, pathfindnode_t *const destinationnode)
{
	boolean reconstructsuccess = false;

	I_Assert(path != NULL);
	I_Assert(destinationnode != NULL);

	{
		size_t numnodes = 0U;
		pathfindnode_t *thisnode = destinationnode;

		// If the path we're placing our new path into already has data, free it
		if (path->array != NULL)
		{
			Z_Free(path->array);
			path->numnodes = 0U;
			path->totaldist = 0U;
		}

		// Do a fast check of how many nodes there are so we know how much space to allocate
		for (thisnode = destinationnode; thisnode; thisnode = thisnode->camefrom)
		{
			numnodes++;
		}

		if (numnodes > 0U)
		{
			// Allocate memory for the path
			path->numnodes  = numnodes;
			path->array     = Z_Calloc(numnodes * sizeof(pathfindnode_t), PU_STATIC, NULL);
			path->totaldist = destinationnode->gscore;
			if (path->array == NULL)
			{
				I_Error("K_ReconstructPath: Out of memory.");
			}

			// Put the nodes into the return array
			for (thisnode = destinationnode; thisnode; thisnode = thisnode->camefrom)
			{
				path->array[numnodes - 1U] = *thisnode;
				// Correct the camefrom element to point to the previous element in the array instead
				if ((path->array[numnodes - 1U].camefrom != NULL) && (numnodes > 1U))
				{
					path->array[numnodes - 1U].camefrom = &path->array[numnodes - 2U];
				}
				else
				{
					path->array[numnodes - 1U].camefrom = NULL;
				}

				numnodes--;
			}

			reconstructsuccess = true;
		}
	}

	return reconstructsuccess;
}

/*--------------------------------------------------
	boolean K_PathfindAStar(path_t *const path, pathfindsetup_t *const pathfindsetup)

		See header file for description.
--------------------------------------------------*/
boolean K_PathfindAStar(path_t *const path, pathfindsetup_t *const pathfindsetup)
{
	boolean pathfindsuccess = false;

	if (path == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "NULL path in K_PathfindAStar.\n");
	}
	else if (pathfindsetup == NULL)
	{
		CONS_Debug(DBG_GAMELOGIC, "NULL pathfindsetup in K_PathfindAStar.\n");
	}
	else if (!K_PathfindSetupValid(pathfindsetup))
	{
		CONS_Debug(DBG_GAMELOGIC, "K_PathfindAStar: Pathfinding setup is not valid.\n");
	}
	else
	{
		pathfindnode_t singlenode = {0};
		singlenode.camefrom  = NULL;
		singlenode.nodedata = pathfindsetup->startnodedata;
		singlenode.heapindex = SIZE_MAX;
		singlenode.hscore    = 0U;
		singlenode.gscore    = 0U;

		if (pathfindsetup->getfinished(&singlenode, pathfindsetup) == true)
		{
			// At the destination, return a simple 1 node path
			K_ReconstructPath(path, &singlenode);
			pathfindsuccess = true;
		}
		else
		{
			bheap_t        openset                 = {0};
			bheapitem_t    poppedbheapitem         = {0};
			pathfindnode_t *nodesarray             = NULL;
			pathfindnode_t **closedset             = NULL;
			pathfindnode_t *newnode                = NULL;
			pathfindnode_t *currentnode            = NULL;
			pathfindnode_t *connectingnode         = NULL;
			void           **connectingnodesdata   = NULL;
			void           *checknodedata          = NULL;
			UINT32         *connectingnodecosts    = NULL;
			size_t         numconnectingnodes      = 0U;
			size_t         connectingnodeheapindex = 0U;
			size_t         nodesarraycount         = 0U;
			size_t         closedsetcount          = 0U;
			// [ZE]: Active lookup capacity can be smaller than the backing buffer.
			size_t         lookupcapacity          = 2U;
			size_t         i                       = 0U;
			UINT32         tentativegscore         = 0U;

			// Set the dynamic structure capacites to defaults if they are 0
			if (pathfindsetup->nodesarraycapacity == 0U)
			{
				pathfindsetup->nodesarraycapacity = DEFAULT_NODEARRAY_CAPACITY;
			}
			if (pathfindsetup->opensetcapacity == 0U)
			{
				pathfindsetup->opensetcapacity = DEFAULT_OPENSET_CAPACITY;
			}
			if (pathfindsetup->closedsetcapacity == 0U)
			{
				pathfindsetup->closedsetcapacity = DEFAULT_CLOSEDSET_CAPACITY;
			}

			// [ZE]: Reuse scratch arrays in place of the per-search zero-filled allocations.
			if (pathfindsetup->nodesarraycapacity > pathfindscratch.nodescapacity)
			{
				Z_Free(pathfindscratch.nodes);
				K_PathfindReserveNodes(pathfindsetup->nodesarraycapacity);
			}
			// [ZE]: Take the retained arrays after any growth.
			nodesarray = pathfindscratch.nodes;
			K_PathfindReserveClosedSet(pathfindsetup->closedsetcapacity);
			closedset = pathfindscratch.closedset;
			while (lookupcapacity / 2U < pathfindsetup->nodesarraycapacity)
			{
				// [ZE]: Use a power of two so probing wraps with a mask.
				lookupcapacity *= 2U;
			}
			// [ZE]: Discard indices from the previous search.
			K_PathfindResetLookup(lookupcapacity, nodesarray, 0U);
			K_BHeapInit(&openset, pathfindsetup->opensetcapacity);

			// Create the first node and add it to the open set
			newnode            = &nodesarray[nodesarraycount];
			newnode->heapindex = SIZE_MAX;
			newnode->nodedata  = pathfindsetup->startnodedata;
			newnode->camefrom  = NULL;
			newnode->gscore    = 0U;
			newnode->hscore    = pathfindsetup->getheuristic(newnode->nodedata, pathfindsetup->endnodedata);
			// [ZE]: Initialize membership and publish the source node's index.
			pathfindscratch.closed[nodesarraycount] = false;
			pathfindscratch.lookup[K_PathfindLookupSlot(nodesarray, newnode->nodedata, lookupcapacity - 1U)] = nodesarraycount + 1U;
			nodesarraycount++;
			K_BHeapPush(&openset, newnode, K_NodeGetFScore(newnode), K_NodeUpdateHeapIndex);

			// update openset capacity if it changed
			if (openset.capacity != pathfindsetup->opensetcapacity)
			{
				pathfindsetup->opensetcapacity = openset.capacity;
			}

			// Go through each node in the openset, adding new ones from each node to it
			// this continues until a path is found or there are no more nodes to check
			while (openset.count > 0U)
			{
				// pop the best node off of the openset
				K_BHeapPop(&openset, &poppedbheapitem);
				currentnode = (pathfindnode_t*)poppedbheapitem.data;

				if (pathfindsetup->getfinished(currentnode, pathfindsetup) == true)
				{
					pathfindsuccess = K_ReconstructPath(path, currentnode);
					break;
				}

				// Place the node we just popped into the closed set, as we are now evaluating it
				if (closedsetcount >= pathfindsetup->closedsetcapacity)
				{
					// [ZE]: Preserve the logical doubling and grow backing storage as needed.
					pathfindsetup->closedsetcapacity = pathfindsetup->closedsetcapacity * 2;
					K_PathfindReserveClosedSet(pathfindsetup->closedsetcapacity);
					closedset = pathfindscratch.closedset;
				}
				closedset[closedsetcount] = currentnode;
				// [ZE]: Record membership when upstream appends the node.
				pathfindscratch.closed[currentnode - nodesarray] = true;
				closedsetcount++;

				// Get the needed data for the next nodes from the current node
				connectingnodesdata = pathfindsetup->getconnectednodes(currentnode->nodedata, &numconnectingnodes);
				connectingnodecosts = pathfindsetup->getconnectioncosts(currentnode->nodedata);

				if (connectingnodesdata == NULL)
				{
					CONS_Debug(DBG_GAMELOGIC, "K_PathfindAStar: A Node returned NULL connecting node data.\n");
				}
				else if (connectingnodecosts == NULL)
				{
					CONS_Debug(DBG_GAMELOGIC, "K_PathfindAStar: A Node returned NULL connecting node costs.\n");
				}
				else
				{
					// For each connecting node add it to the openset if it's unevaluated and not there,
					// skip it if it's in the closedset or not traversable
					for (i = 0; i < numconnectingnodes; i++)
					{
						checknodedata = connectingnodesdata[i];

						if (checknodedata == NULL)
						{
							CONS_Debug(DBG_GAMELOGIC, "K_PathfindAStar: A Node has a NULL connecting node.\n");
						}
						else
						{
							// skip this node if it isn't traversable
							if (pathfindsetup->gettraversable(checknodedata, currentnode->nodedata) == false)
							{
								continue;
							}

							// Figure out what the gscore of this route for the connecting node is
							tentativegscore = currentnode->gscore + connectingnodecosts[i];

							// [ZE]: Look up the existing node by data pointer, as the reverse scan did.
							size_t lookupslot = K_PathfindLookupSlot(nodesarray, checknodedata, lookupcapacity - 1U);
							const size_t storedindex = pathfindscratch.lookup[lookupslot];
							connectingnode = storedindex ? &nodesarray[storedindex - 1U] : NULL;

							if (connectingnode != NULL)
							{
								// The connecting node has been seen before, so it must be in either the closedset (skip it)
								// or the openset (re-evaluate it's gscore)
								// [ZE]: The flag tracks the closed set without scanning it.
								if (pathfindscratch.closed[storedindex - 1U] == true)
								{
									continue;
								}
								else if (tentativegscore < connectingnode->gscore)
								{
									// The node is not in the closedset, update it's gscore if this path to it is faster
									connectingnode->gscore   = tentativegscore;
									connectingnode->camefrom = currentnode;

									connectingnodeheapindex =
										K_BHeapContains(&openset, connectingnode, connectingnode->heapindex);
									if (connectingnodeheapindex != SIZE_MAX)
									{
										K_UpdateBHeapItemValue(
											&openset.array[connectingnodeheapindex], K_NodeGetFScore(connectingnode));
									}
									else
									{
										// SOMEHOW the node is not in either the closed set OR the open set
										CONS_Debug(DBG_GAMELOGIC, "K_PathfindAStar: A Node is not in either set.\n");
									}
								}
							}
							else
							{
								// Node is not created yet, so it hasn't been seen so far
								// Reallocate nodesarray if it's full
								if (nodesarraycount >= pathfindsetup->nodesarraycapacity)
								{
									// [ZE]: Preserve the logical doubling even if the backing array already fits.
									pathfindsetup->nodesarraycapacity = pathfindsetup->nodesarraycapacity * 2;
									K_PathfindReserveNodes(pathfindsetup->nodesarraycapacity);

									// [ZE]: Copy live nodes and fix their pointers before freeing the old array.
									if (nodesarray != pathfindscratch.nodes)
									{
										pathfindnode_t *nodesarrayrealloc = pathfindscratch.nodes;
										size_t j = 0U;
										size_t arrayindex = 0U;
										// [ZE]: Only entries that belong to this search are live.
										M_Memcpy(nodesarrayrealloc, nodesarray, nodesarraycount * sizeof(*nodesarray));
										for (j = 0U; j < closedsetcount; j++)
										{
											arrayindex = closedset[j] - nodesarray;
											closedset[j] = &nodesarrayrealloc[arrayindex];
										}
										for (j = 0U; j < openset.count; j++)
										{
											arrayindex = ((pathfindnode_t *)(openset.array[j].data)) - nodesarray;
											openset.array[j].data = &nodesarrayrealloc[arrayindex];
										}
										for (j = 0U; j < nodesarraycount; j++)
										{
											if (nodesarrayrealloc[j].camefrom != NULL)
											{
												arrayindex = nodesarrayrealloc[j].camefrom - nodesarray;
												nodesarrayrealloc[j].camefrom = &nodesarrayrealloc[arrayindex];
											}
										}

										// [ZE]: Fix the current pointer before releasing the old storage too.
										arrayindex = currentnode - nodesarray;
										currentnode = &nodesarrayrealloc[arrayindex];

										// [ZE]: All retained pointers now refer to the new array.
										Z_Free(nodesarray);
										nodesarray = nodesarrayrealloc;
									}
								}

								// [ZE]: Keep at least one empty slot for every live node.
								if (nodesarraycount >= lookupcapacity / 2U)
								{
									lookupcapacity *= 2U;
									K_PathfindResetLookup(lookupcapacity, nodesarray, nodesarraycount);
									lookupslot = K_PathfindLookupSlot(nodesarray, checknodedata, lookupcapacity - 1U);
								}

								// Create the new node and add it to the nodes array and open set
								newnode            = &nodesarray[nodesarraycount];
								newnode->heapindex = SIZE_MAX;
								newnode->nodedata  = checknodedata;
								newnode->camefrom  = currentnode;
								newnode->gscore    = tentativegscore;
								newnode->hscore    = pathfindsetup->getheuristic(newnode->nodedata, pathfindsetup->endnodedata);
								// [ZE]: Initialize membership and publish the new node's index.
								pathfindscratch.closed[nodesarraycount] = false;
								pathfindscratch.lookup[lookupslot] = nodesarraycount + 1U;
								nodesarraycount++;
								K_BHeapPush(&openset, newnode, K_NodeGetFScore(newnode), K_NodeUpdateHeapIndex);
							}
						}
					}
				}
			}

			// [ZE]: Free the heap as before and retain the other arrays for the next search.
			K_BHeapFree(&openset);
		}
	}

	return pathfindsuccess;
}
