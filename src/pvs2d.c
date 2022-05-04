#include "pvs2d.h"

#include <stdlib.h>

/*
segments have pointer to line they are based on
lines contains segments that are based on this line and tStart and tEnd. tStart < tEnd
bsp nodes should contain the splitting line, left and right childs or leaves indices
portals are segs and have index of leaves to the left and right
building BSP:
	firstly group all given segments onto lines:
		have a stack of lines
		for each new seg iter through all lines and if match then assign this seg to its line and viseversa
		if not then just add new line
	have a list of all segments in the area
	choose any line it has, see the amount of segments it cuts
	choose the line that has minimum cuts
	separate all segment to three lists: to the left of the line, to the right and adjacent
	put all adjacent into the current node
	create nodes for left and right if needed and recursively repeat the process

building portals:
	initially create bunch of portals
	for each node:
		have a list of adjacent portals to subspace
		if a node is a leaf, then check for errors in lists and fill in portals in list with leaf id
		otherwise
		split the line of node into portals? each node should have boundaries of splitline
		add those portals to left list and right list
		split the portals in the given list to left and right, potentially splitting some of them?
building PVS:
	there are two options of how to do this:
	first is to use the O(num_of_transparent^2 * num_of_opaque)
	second is to build a graph of leaf adjacency, where leafs are nodes and visible portals are edges.
	for each leaf launch DFS: firstly add to PVS all the adjacent leafs,
		then on each leaf during DFS, have previous edge, for each edge of the current leaf:
			calculate whether the current edge is visible in intersections of all frustums in stack
			if yes, build 2d view frustum between prev edge and current edge,
				and put it into stack and enter the node, and remove after exiting it
			otherwise skip the edge
	this solution should work for around
	O(num_of_leaves * (num_of_leaves + num_of_transparent) * num_of_transparent * O(is_segment_in_frustum))
	in the worst case. However consider the fact that most of the times DFS won't go into the branch
	either because its already calculated or frustum check failed,
	so the amount of times is_segment_in_frustum is called is minimized significantly
	also this method is cooler
	the Drawback is that we never considering the opaque walls, which, if we use the first method,
	trim PVS a little bit, so we have slightly more. the Drawdrawback of this Drawback is that it is
	insignificant if we use portal-based frustum culling
*/

#ifndef DBG_ASSERT
#ifdef _DEBUG
#include <stdio.h>
#include <math.h>
#define DBG_ASSERT(x, ...) { if (!(x)) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return -1; }}
#else
#define DBG_ASSERT(x, ...)
#endif
#endif

#define MATCH_TOLERANCE (0.0625f)
// the so called EPS. used to fix some errors that inevitably happen with float arithmetics

static inline char _collinear(int ax, int ay, int bx, int by, int cx, int cy) {
	return (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by) == 0);
};

static inline void _intersect(int ax, int ay, int bx, int by, int cx, int cy, int dx, int dy, int* numerDest, int* denomDest) {
	int nx = bx - ax, ny = by - ay;
	*numerDest = nx * (cy - ay) - ny * (cx - ax);
	*denomDest = nx * (cy - dy) - ny * (cx - dx);
};

// returns 0 if vector (ax, ay) is on the right side of (bx, by)
// returns anything else if not
static inline unsigned int _side(float ax, float ay, float bx, float by) {
	return (ax * by > ay * bx);
}

int _cropSplitSegs(PVS2D_BSPTreeNode* node, PVS2D_Line* line, int left) {
	if (node->line == line) {
		DBG_ASSERT(0, "This should not have happened...");
	}
	int numer, denom;
	_intersect(
		line->ax, line->ay, line->bx, line->by,
		node->line->ax, node->line->ay, node->line->bx, node->line->by,
		&numer, &denom
	);
	if (denom == 0) {
		// parallel. do nothing
		return 0;
	}
	float t = (float)numer / denom;
	if (left) {
		// crop the splitseg so it is to the left
		if (denom > 0) {
			node->tSplitEnd = min(t, node->tSplitEnd);
		}
		else {
			node->tSplitStart = max(t, node->tSplitStart);
		}
	}
	else {
		// crop the splitseg so it is to the right
		if (denom > 0) {
			node->tSplitStart = max(t, node->tSplitStart);
		}
		else {
			node->tSplitEnd = min(t, node->tSplitEnd);
		}
	}
	if (node->left) {
		_cropSplitSegs(node->left, line, left);
	}
	if (node->right) {
		_cropSplitSegs(node->right, line, left);
	}
}

enum _side {
	SIDE_COL,			// collinear
	SIDE_L_PARAL,		// parallel, is on the left
	SIDE_R_PARAL,		// parallel, is on the right
	SIDE_S_FL,			// faces left, splitted
	SIDE_S_FR,			// faces right, splitted
	SIDE_L_FL,			// faces left, is on the left
	SIDE_L_FR,			// faces right, is on the left
	SIDE_R_FL,			// faces left, is on the right
	SIDE_R_FR			// faces right, is on the right
};

// tells which side of line the segment is on, as well as what orientation it has.
// the orientation is being treated as if we would stand in line's point A looked 
// on the point B also, if the those bits aren't 00, then the function can output the 
// parameter of point on the segment's line of where the collision happens.
char _split(PVS2D_Line* line, PVS2D_Seg* seg, float* tDest) {
	if (seg->line == line) {
		// collinear.
		return SIDE_COL;
	}
	int numer, denom;
	_intersect(
		line->ax, line->ay, line->bx, line->by,
		seg->line->ax, seg->line->ay, seg->line->bx, seg->line->by,
		&numer, &denom
	);
	if (denom == 0) {
		// parallel.
		if (numer == 0) {
			// they are still collinear
			// this should probably be a warning
			return SIDE_COL;
		}
		else if (numer > 0) {
			// to the left
			return SIDE_L_PARAL;
		}
		else {
			// to the right
			return SIDE_R_PARAL;
		}
	}
	else {
		// not parallel. calculate the split point
		float t = (float)numer / denom;
		if (tDest) *tDest = t;
		
		if (t > seg->tStart + MATCH_TOLERANCE && t < seg->tEnd - MATCH_TOLERANCE) {
			// splits the segment
			if (denom < 0) {
				// faces left
				return SIDE_S_FL;
			}
			else {
				// faces right
				return SIDE_S_FR;
			}
		}
		else {
			// doesn't split the segment.
			// in order to get more accurate result, we will use middle point of the segment
			// to determine the side it is on
			float middle = (seg->tStart + seg->tEnd) / 2;
			// we need to take in account the orientation of the segment in order for our magic to work
			// the rest here are mostly self-explainatory
			if (denom < 0) {
				if (middle > t) {
					return SIDE_L_FL;
				}
				else {
					return SIDE_R_FL;
				}
			}
			else {
				if (middle > t) {
					return SIDE_R_FR;
				}
				else {
					return SIDE_L_FR;
				}
			}
		}
	}

};

int _buildBSP(PVS2D_BSPTreeNode* cur_node, PVS2D_SegStack* cur_segs) {
	DBG_ASSERT(cur_node, "cur_node can't be NULL (node must be allocated before calling the function)")
	DBG_ASSERT(cur_segs, "cur_segs can't be NULL (segment array can't have 0 segments)");
	cur_node->left = 0;
	cur_node->right = 0;
	cur_node->leftLeaf = 0;
	cur_node->rightLeaf = 0;
	cur_node->line = 0;
	cur_node->segs = 0;
	cur_node->tSplitStart = -INFINITY;
	cur_node->tSplitEnd = INFINITY;
	cur_node->portals = 0;


	PVS2D_Seg* rootSeg = 0;
	unsigned int minsplits = -1;        // UINT_MAX
	static int leafIndex = 1;
	// the index of leaves, tells the amount of leaves compiled.
	// the static is used instead of global variable to reduce junk.
	// also note that leaf indices count from 1 instead of 0.
	// this was done in order to not use uint max for the same thing, 
	// so the leaf 0 is 'invalid' leaf

	for (PVS2D_SegStack* rootHead = cur_segs; rootHead != 0; rootHead = rootHead->next) {
		// choose any segment and see how much it splits
		unsigned int splitC = 0;
		for (PVS2D_SegStack* curHead = cur_segs; curHead != 0; curHead = curHead->next) {
			char side = _split(rootHead->seg->line, curHead->seg, 0);
			if (side == SIDE_S_FL || side == SIDE_S_FR) {
				// it splits it
				splitC++;
			}
		}
		if (splitC < minsplits) {
			minsplits = splitC;
			rootSeg = rootHead->seg;
		}
	}

	// use the min segment and split all segments into right ones, left ones and etc.
	PVS2D_SegStack* segsLeft = 0;
	PVS2D_SegStack* segsRight = 0;
	cur_node->line = rootSeg->line;
	PVS2D_SegStack* curHead, * nextHead = cur_segs;
	while (1) {
		if (nextHead == 0) {
			break;
		}
		curHead = nextHead;
		nextHead = curHead->next;
		// this juncture above was used instead of for loop to allow us modifying curHead->next ptr 

		float t;
		char side = _split(rootSeg->line, curHead->seg, &t);

		switch (side) {
		case SIDE_COL:;
			curHead->next = cur_node->segs;
			cur_node->segs = curHead;
			break;
		case SIDE_L_PARAL:;
		case SIDE_L_FL:;
		case SIDE_L_FR:;
			curHead->next = segsLeft;
			segsLeft = curHead;
			break;
		case SIDE_R_PARAL:;
		case SIDE_R_FL:;
		case SIDE_R_FR:;
			curHead->next = segsRight;
			segsRight = curHead;
			break;
		case SIDE_S_FL:;
		case SIDE_S_FR:;
			PVS2D_SegStack* newElem = (PVS2D_SegStack*)malloc(sizeof(PVS2D_SegStack));
			DBG_ASSERT(newElem, "Failed to allocate new seg stack node");
			*newElem = *curHead;
			newElem->seg = (PVS2D_Seg*)malloc(sizeof(PVS2D_Seg));
			DBG_ASSERT(newElem->seg, "Failed to allocate new segment");
			*newElem->seg = *curHead->seg;
			newElem->seg->tStart = t;
			curHead->seg->tEnd = t;

			newElem->next = (side == SIDE_S_FL) ? segsLeft : segsRight;
			curHead->next = (side == SIDE_S_FL) ? segsRight : segsLeft;
			segsLeft  = (side == SIDE_S_FL) ? newElem : curHead;
			segsRight = (side == SIDE_S_FL) ? curHead : newElem;
			break;
		default:
			// something is wrong here
			break;
		}

	}
	// now all segments are sorted to their lists. 
	// now we also have to calculate tSplitStart and tSplitEnd
	// this is done by each node descending its children and "cutting" their split segment by itself
	// time for recursion
	if (segsLeft == 0) {
		// we don't have any segs to the left, therefore its a leaf.
		cur_node->left = 0;
		cur_node->leftLeaf = leafIndex++;
	}
	else {
		PVS2D_BSPTreeNode* newNode = (PVS2D_BSPTreeNode*)malloc(sizeof(PVS2D_BSPTreeNode));
		DBG_ASSERT(newNode, "Failed to allocate new BSP tree node");
		int rez = _buildBSP(newNode, segsLeft);
		if (rez != 0) return rez;   // error encountered
		cur_node->left = newNode;
		cur_node->leftLeaf = 0;
		// crop left children
		_cropSplitSegs(newNode, cur_node->line, 1);
	}

	if (segsRight == 0) {
		// we don't have any segs to the right, therefore its a leaf
		cur_node->right = 0;
		cur_node->rightLeaf = leafIndex++;
	}
	else {
		PVS2D_BSPTreeNode* newNode = (PVS2D_BSPTreeNode*)malloc(sizeof(PVS2D_BSPTreeNode));
		DBG_ASSERT(newNode, "Failed to allocate new BSP tree node");
		int rez = _buildBSP(newNode, segsRight);
		if (rez != 0) return rez;   // error encountered
		cur_node->right = newNode;
		cur_node->rightLeaf = 0;
		// crop right children
		_cropSplitSegs(newNode, cur_node->line, 0);
	}
	// nothing seems needs freeing.

	return 0;

};

int PVS2D_BuildBSPTree(int* segs, unsigned int segsC, PVS2D_BSPTreeNode* rootDest) {
	typedef struct _lstack {
		PVS2D_Line* line;
		struct _lstack* next;
	} _lstack;

	PVS2D_SegStack* prSegs = 0;
	_lstack* prLines = 0;
	for (unsigned int i = 0; i < segsC; i++) {
		int ax = segs[5 * i],
			ay = segs[5 * i + 1],
			bx = segs[5 * i + 2],
			by = segs[5 * i + 3],
			opq = segs[5 * i + 4];
		_lstack* match = 0;
		for (_lstack* top = prLines; top != 0; top = top->next) {
			if (_collinear(ax, ay, bx, by, top->line->ax, top->line->ay) &&
				_collinear(ax, ay, bx, by, top->line->bx, top->line->by)
			) {
				match = top;
				break;
			}
		}
		PVS2D_SegStack* newSeg = (PVS2D_SegStack*)malloc(sizeof(PVS2D_SegStack));
		DBG_ASSERT(newSeg, "Failed to allocate new segment stack node");
		newSeg->seg = (PVS2D_Seg*)malloc(sizeof(PVS2D_Seg));
		DBG_ASSERT(newSeg->seg, "Failed to allocate new segment");
		float tStart = 0.0f, tEnd = 1.0f;
		if (match == 0) {
			_lstack* newLine = (_lstack*)malloc(sizeof(_lstack));
			DBG_ASSERT(newLine, "Failed to allocate new line into stack");
			newLine->line = (PVS2D_Line*)malloc(sizeof(PVS2D_Line));
			DBG_ASSERT(newLine->line, "Failed to allocate new line in stack node");
			newLine->line->ax = ax;
			newLine->line->ay = ay;
			newLine->line->bx = bx;
			newLine->line->by = by;
			newLine->line->mems = (PVS2D_SegStack*)malloc(sizeof(PVS2D_SegStack));
			DBG_ASSERT(newLine->line->mems, "Failed to allocate new segment into lines's stack");
			newLine->line->mems->seg = newSeg->seg;
			newLine->line->mems->next = 0;
			newLine->next = prLines;
			prLines = newLine;

			newSeg->seg->line = newLine->line;
		}
		else {
			if (ax == bx) {
				tStart = (float)(ay - match->line->ay) / (match->line->by - match->line->ay);
				tEnd = (float)(by - match->line->ay) / (match->line->by - match->line->ay);
			}
			else {
				tStart = (float)(ax - match->line->ax) / (match->line->bx - match->line->ax);
				tEnd = (float)(bx - match->line->ax) / (match->line->bx - match->line->ax);
			}
			if (tStart > tEnd) {
				float _t = tStart;
				tStart = tEnd;
				tEnd = _t;
			}
			newSeg->seg->line = match->line;
			PVS2D_SegStack* newEntry = (PVS2D_SegStack*)malloc(sizeof(PVS2D_SegStack));
			DBG_ASSERT(newEntry, "Failed to allocate new entry to global seg stack");
			newEntry->seg = newSeg->seg;
			newEntry->next = match->line->mems;
			match->line->mems = newEntry;
		}
		newSeg->seg->tStart = tStart;
		newSeg->seg->tEnd = tEnd;
		newSeg->seg->opq = opq;
		newSeg->next = prSegs;
		prSegs = newSeg;
	}
	// free line stack (but not the lines themselves)
	for (_lstack* prLine = prLines; prLine != 0;) {
		_lstack* next = prLine->next;
		free(prLine);
		prLine = next;
	}
	return _buildBSP(rootDest, prSegs);

};

unsigned int PVS2D_FindLeafOfPoint(PVS2D_BSPTreeNode* root, float x, float y) {
	if (_side(
		root->line->bx - root->line->ax,
		root->line->by - root->line->ay,
		x - root->line->ax,
		y - root->line->ay)
	) {
		// its on the left
		return (root->left) ? PVS2D_FindLeafOfPoint(root->left, x, y) : root->leftLeaf;
	}
	else {
		// its on the right
		return (root->right) ? PVS2D_FindLeafOfPoint(root->right, x, y) : root->rightLeaf;
	}
}
