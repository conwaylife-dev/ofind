﻿/*
** ofind - search for oscillators in semitotalistic cellular automata
** David Eppstein, UC Irvine, 5 April 2000
**
** For usage help, run this program and type a question mark at the prompts.
**
** As in gfind, we use a hybrid breadth first / depth first search, but
** with a state space in which each step adds rows in all phases at once.
** To do this, we find for each phase a set of rows which produce the
** correct evolution in the following phase, build a graph representing
** compatibility relations between rows from successive phases (where two
** rows are compatible if one can be made to evolve into the next) and
** search for cycles in the graph formed by one row from each phase.
**
** History:
** 0.9, Aug 2001:
**    Search for still lifes when period=1
**    Fix bug in makeNewState causing duplicate states to remain in search tree
**    Bump NCOMPAT, was running out for big still-life searches
*/

#include <stdio.h>
#include <stdlib.h>

// this was defined to abstract rng implementation in legacy platforms
#define random() rand()

void exit(int);
static void failure();

/* define DEBUG */

typedef enum { none, odd, even } sym_type;
sym_type symmetry = even;
sym_type row_symmetry;
int row_sym_phase_offset;
int allow_row_sym = 1;
int rule = 010014;
int period = 5;
int rotorWidth = 4;
int leftStatorWidth = 0;
int rightStatorWidth = 0;
int maxDeepen = 0;
int hashing = 1;
int sparkLevel = 0;
int zeroLotLine = 0;
#define totalWidth (rotorWidth+leftStatorWidth+rightStatorWidth)
#define MAXPERIOD 20

/* ============================================================== */
/*  Behave nicely under non-preemptive multitasking (i.e. MacOS)  */
/* ============================================================== */

#ifdef __MWERKS__

#include <Events.h>
#include <SIOUX.h>

long niceTimer = 0;
#define NICE() if (--niceTimer <= 0) beNice()

/* global timer value - call beNice every k iterations */
#define TOTALNICENESS (1<<16)

static void beNice()
{
	EventRecord evRec;
	niceTimer = TOTALNICENESS;
	while (WaitNextEvent(-1, &evRec, 0, 0))
		SIOUXHandleOneEvent(&evRec);
}

#else
#define NICE()
#endif

/* ========================== */
/*  Representation of states  */
/* ========================== */

#include <inttypes.h>

typedef int32_t State;
typedef uint32_t Row;

#define STATE_SPACE_SIZE (INT32_MAX)
Row * statespace; /* Will be allocated later. */
State firstUnprocessedState;
State firstFreeState;
#define firstState (0)
#define lastState (STATE_SPACE_SIZE)
#define queueFull (STATE_SPACE_SIZE/2)

static inline State parentState(State s)
{
	return statespace[s];
}

static inline void setParentState(State s, State parent)
{
	statespace[s] = parent;
}

static inline Row rowOfState(State s, int phase)
{
	return statespace[s + 1 + phase];
}


static inline void setRowOfState(State s, int phase, Row row)
{
	statespace[s + 1 + phase] = row;
}

static State nextState(State s)
{
	State l = s + period + 1;
	if (l >= lastState) {
		printf("Queue full, aborting!\n");
		failure();
	}
	return l;
}

static State previousState(State s)
{
	return s - (period + 1);
}

static void makeInitialStates(void)
{
	int phase;
	statespace = calloc(STATE_SPACE_SIZE, sizeof *statespace);
	firstUnprocessedState = firstState;
	setParentState(firstUnprocessedState, firstUnprocessedState);
	for (phase = 0; phase < period; phase++)
		setRowOfState(firstUnprocessedState, phase, 0);
	firstFreeState = nextState(firstUnprocessedState);
}

/* ============================================ */
/*  Hash table for duplicate state elimination  */
/* ============================================ */

#define HASHSIZE (1<<21)
#define HASHMASK (HASHSIZE - 1)
State hashTable[HASHSIZE];

long hashValTab[MAXPERIOD*1024];
long hashValPTab[MAXPERIOD*1024];
#define HASHIDX(p,b,s) ((p)<<10)+(b<<8)+((rowOfState(s, p)>>(b*8))&0xff)
#define HASHBYTE(phase,byte,s) (hashValTab[HASHIDX(phase,byte,s)]+hashValPTab[HASHIDX(phase,byte,parentState(s))])

static void clearHash() {
	int i;
	for (i = 0; i < HASHSIZE; i++) hashTable[i] = 0;
}

static void initHash() {
	int i;
	clearHash();
	for (i = 0; i < MAXPERIOD*1024; i++) {
		hashValTab[i] = random();
		hashValPTab[i] = random();
	}
}

static int isDuplicate(State s, State t) {
	State ps = parentState(s);
	State pt = parentState(t);
	int phase;
	for (phase = 0; phase < period; phase++)
		if (rowOfState(s, phase) != rowOfState(t, phase) ||
			 rowOfState(ps, phase) != rowOfState(pt, phase)) return 0;
	return 1;
}

/* hash a value, return nonzero if not hashed because duplicate exists */
static int hash(State s)
{
	long hashKey = 0;
	int nTries = 3;
	int phase;

	/* compute hash key */
	for (phase = 0; phase < period; phase++)
		hashKey += HASHBYTE(phase,0,s) + HASHBYTE(phase,1,s) +
					  HASHBYTE(phase,2,s) + HASHBYTE(phase,3,s);

	/* attempt to locate blank spot or duplicate */
	while (nTries-- > 0) {
		if (hashTable[hashKey & HASHMASK] == 0) {
			hashTable[hashKey & HASHMASK] = s;
			return 0;	/* successfully hashed */
		} else if (isDuplicate(s, hashTable[hashKey & HASHMASK])) return 1;
		hashKey += (hashKey >> 16);
	}
	return 0;	/* unable to find blank, ignore but dont treat as a duplicate */
}

/* ================================================================ */
/*  Make structure representing possible extension rows x,a,b -> c  */
/* ================================================================ */

/*
** Structure consists of an array of ints, each a bitmap representing
** the allowable states of three consecutive cells of x.  Mapping cells->bits
**    000 -> 01
**    100 -> 02
**    010 -> 04
**    110 -> 010
**    001 -> 020
**    101 -> 040
**    011 -> 0100
**    111 -> 0200
** The first cell in extensions[i] corresponds to row&(1<<i).
*/

int extensions[32];
int downShifts[256];
static void makeDownShifts(void) {
	int x;
	for (x = 0; x < 0377; x++) {
		int y = 0;
		if (x & 021) y |= 03;
		if (x & 042) y |= 014;
		if (x & 0104) y |= 060;
		if (x & 0210) y |= 0300;
		downShifts[x] = y;
	}
}
#define downShift(x) downShifts[x]

#define EXTBITS (8+3+3+1)
#define NEXTTAB (1<<EXTBITS)
#define EXTIDX(x,a,b,c) (((x)<<7) | (((a)&7)<<4) | (((b)&7)<<1) | (((c)&2)>>1))
int extTab[NEXTTAB];
#define nextExtension(x,a,b,c) extTab[EXTIDX(x,a,b,c)]
#define maskedExtension(x,a,b,c,m) (extTab[EXTIDX(x,a,b,c)]&extTab[(m)&(EXTIDX(x,a,b,c))])

static void makeExtTab(void)
{
	int base,x,a,b,c;
	for (x = 0; x < NEXTTAB; x++) extTab[x] = 0;
	for (base = 0; base <= 255; base++)
		for (x = 0; x <= 15; x++)
			if (base & (1<<(x&7)))
				for (a = 0; a <= 7; a++)
					for (b = 0; b <= 7; b++) {
						int ruleBit = 9;
						if (a & 1) ruleBit++;
						if (a & 2) ruleBit -= 9;
						if (a & 4) ruleBit++;
						if (b & 1) ruleBit++;
						if (b & 2) ruleBit++;
						if (b & 4) ruleBit++;
						if (x & 2) ruleBit++;
						if (x & 4) ruleBit++;
						if (x & 8) ruleBit++;
						c = ((rule>>ruleBit) & 1)<<1;
						nextExtension(base,a,b,c) |= (1<<(x>>1));
					}
}

static void setupExtensions(Row a, Row b, Row c, Row sparkMask)
{
	int x, i;
	switch (symmetry) {
		case none:
			x = 1;
			x = maskedExtension(x,a<<2,b<<2,c<<2,sparkMask);
			x = maskedExtension(x,a<<1,b<<1,c<<1,sparkMask);
			break;
		case odd:
			x = 0377;
			x = maskedExtension(x,(a<<1) | ((a&2)>>1), (b<<1) | ((b&2)>>1), c<<1, sparkMask);
			x &= 0245;	/* keep symmetric states only */
			break;
		case even:
			x = 0303;	/* start w/symmetric states only */
			x = maskedExtension(x, (a<<1) | (a&1), (b<<1) | (b&1), c<<1, sparkMask);
			break;
		default:
			__builtin_unreachable();
	}
	for (i = 0; i < totalWidth; i++) {
		extensions[i] = x = maskedExtension(x,a,b,c,sparkMask);
		a >>= 1;
		b >>= 1;
		c >>= 1;
	}
}

/* ====================================================================== */
/*  Make list of possible extension rows given info from setupExtensions  */
/* ====================================================================== */

#define NROWS (1<<20) /* 4 megabytes */
Row rows[NROWS];
int firstRow[MAXPERIOD];
int nRows[MAXPERIOD];

static void listRows(Row partialRow, int phase, int bit, int extension)
{
	if (extension != 0) {
		if (bit < 0) {	/* found a full matching row */
			rows[firstRow[phase] + (nRows[phase]++)] = partialRow;
			if (firstRow[phase] + nRows[phase] >= NROWS) {
				printf("max number of new rows/state exceeded, aborting\n");
				failure();
			}
			NICE();
		} else {
			extension &= extensions[bit];
			listRows(partialRow, phase, bit-1, downShift(extension & 0125));
			listRows(partialRow+(1<<bit), phase, bit-1, downShift(extension & 0252));
		}
	}
}

int rowIndices[MAXPERIOD];
#define STATMASK ((((1<<rightStatorWidth)-1)<<(rotorWidth+leftStatorWidth))|((1<<leftStatorWidth)-1))

/* =============================================== */
/*  Test and store compatibility of pairs of rows  */
/* =============================================== */

#define NCOMPAT (1<<21)
Row compatBits[NCOMPAT];
int firstCompat[MAXPERIOD];
int compatBlockLength[MAXPERIOD];

static void testCompatible(int phase, int prevRowIndex, int rowIndex, State s)
{
	Row * b;
	int prevPhase = phase - 1;
	if (prevPhase < 0) prevPhase = period - 1;
	if (rowIndex == firstRow[phase]) {
		if (phase == 0) firstCompat[0] = 0;
		else firstCompat[phase] = firstCompat[prevPhase] +
							(compatBlockLength[prevPhase]*nRows[prevPhase]);
		compatBlockLength[phase] = (nRows[prevPhase]+31)>>5;
		if (firstCompat[phase]+(compatBlockLength[phase]*nRows[phase]) > NCOMPAT) {
			fprintf(stderr,"Compatibility block space exceeded, aborting.\n");
			failure();
		}
	}
	b = compatBits + firstCompat[phase] + (compatBlockLength[phase]*(rowIndex-firstRow[phase]));
	if (prevRowIndex == firstRow[prevPhase]) {	/* clear bits if first test for rowIndex */
		int i;
		for (i = 0; i < compatBlockLength[phase]; i++) b[i] = 0;
	}

	/* do stators match? */
	if ((rows[prevRowIndex] & STATMASK) != (rows[rowIndex] & STATMASK)) return;

	/* passed stator check, now check for ok evolution */
	setupExtensions(rows[prevRowIndex],rowOfState(s, prevPhase),rows[rowIndex],-1L);
	if (03 & extensions[totalWidth - 1]) {	/* path exists? */
		int i = prevRowIndex - firstRow[prevPhase];
		b[i>>5] |= ((Row) 1) << (i&037);
	}
}

static int compatible(int phase, int prevRowIndex, int rowIndex)
{
	int i;
	Row * b;
	int prevPhase = phase - 1;
	if (prevPhase < 0) prevPhase = period - 1;
	b = compatBits + firstCompat[phase] + (compatBlockLength[phase]*(rowIndex-firstRow[phase]));
	i = prevRowIndex - firstRow[prevPhase];
	return b[i>>5] & (((Row) 1) << (i & 037));
}

/* ======================================================================= */
/*  Test and store information about which rows in phase 0 can be reached  */
/* ======================================================================= */

#define REACHLENGTH ((nRows[0]+31)>>5)
Row reachBits[NCOMPAT];
int firstReach[MAXPERIOD];

static long reachable(int phase, int firstRowIndex, int rowIndex) {
	return reachBits[firstReach[phase] + (rowIndex*REACHLENGTH) + (firstRowIndex>>5)]
		& (((Row) 1) << (firstRowIndex & 037));
}

static void testReachable()
{
	int phase;
	int i, j, k;

	/* start w/last phase */
	firstReach[period-1] = 0;
	for (i = 0; i < nRows[period-1]; i++) {
		for (j = 0; j < REACHLENGTH; j++) reachBits[i*REACHLENGTH + j] = 0;
		for (j = 0; j < nRows[0]; j++)
			if (compatible(0, firstRow[period-1]+i, firstRow[0]+j)) {
				reachBits[i*REACHLENGTH + (j>>5)] |= (((Row)1) << (j & 037));
			}
	}

	/* now do all remaining phases */
	for (phase = period-2; phase >= 0; phase--) {
		firstReach[phase] = firstReach[phase+1] + nRows[phase+1]*REACHLENGTH;
		if (firstReach[phase] + nRows[phase]*REACHLENGTH >= NCOMPAT) {
			printf("Reachability block storage exceeded, aborting\n");
			failure();
		}
		for (i = 0; i < nRows[phase]; i++) {
			int idx = firstReach[phase] + i*REACHLENGTH;
			for (j = 0; j < REACHLENGTH; j++) reachBits[idx+j] = 0;
			for (j = 0; j < nRows[phase+1]; j++)
				if (compatible(phase+1, firstRow[phase]+i, firstRow[phase+1]+j)) {
					for (k = 0; k < REACHLENGTH; k++)
						reachBits[idx+k] |= reachBits[firstReach[phase+1] + j*REACHLENGTH + k];
				}
		}
	}
}


/* ================================================ */
/*  Detection and output of successful oscillators  */
/* ================================================ */

/* vars for stator termination detection */
#define ODDEXT(r) ((r<<1)|((r&2)>>1))
#define EVEXT(r) ((r<<1)|(r&1))
unsigned short revTerm[1<<16];
Row count[8];
unsigned short nxTerm[1<<22];
unsigned short initialTermState;
int addlStatorCols;

/* output single cell of a found pattern */
static void putCell(Row row, int bit) {
	putchar(row & (1L<<bit)? 'o' : '.');
}

/* output single row of a found pattern */
static void putRow(Row theRow)
{
	int bit;
	for (bit = 0; bit < addlStatorCols; bit++) putchar('.');
	switch(symmetry) {
		case none:
			if (addlStatorCols == 0) putchar('.'); /* hack to fix bad output alignment */
			break;
		case odd:
			for (bit = totalWidth-1; bit > 0; bit--) putCell(theRow,bit);
			break;
		case even:
			for (bit = totalWidth-1; bit >= 0; bit--) putCell(theRow,bit);
			break;
	}
	for (bit = 0; bit <= totalWidth+addlStatorCols-1; bit++) putCell(theRow,bit);
	putchar('\n');
}

/* format of termState (unsigned short):
 * bit b1*8+b2*4+b3*2+b4*1 represents a block
 *
 *    b1  b2
 *    b3  b4
 *    rr  rr
 *
 * where the rr's are parts of the rotor.
 * so to reverse a state, we just need to swap b1<->b2 and b3<->b4
 */

#define NEXTTERM(t,r,pr,sr,i) NXTERM(t,((r)>>i)&7,count[((pr)>>i)&7],((sr)>>(i+1))&1)
#define NXTERM(t,r,pr,sr) nxTerm[(t) | ((r)<<19) | (pr) | ((sr) << 16)]

/* does this row have a subperiod? */
/* linear time algorithm borrowed from KMP string matching method */
static int aperiodic(State s) {
	int i; /* last index of a prefix w of s */
	int p[MAXPERIOD]; /* last index of longest word that's both a prefix and suffix of w */

	if (period == 1)	/* still life wants nonempty rather than aperiodic */
		return (rowOfState(s, 0) != 0);

	p[0] = -1;
	for (i = 1; i < period; i++) {
		p[i] = p[i-1]+1;
		while (rowOfState(s, p[i]) != rowOfState(s, i))
			if (p[i] == 0) {
				p[i] = -1;
				break;
			} else p[i] = p[p[i]-1]+1;
	}
	/* now set i to min possible period length and test if it divides full period */
	i = period - (p[period-1] + 1);
	return (i == period || ((period % i) != 0));
}

/* test whether a state can be concluded */
static int terminal(State s) {
	int i;
	int phase;
	State ps = parentState(s);
	unsigned short term = initialTermState;
	unsigned short nextTerm;

	row_symmetry = none;
	if (ps == s) return 0;	/* initial state is not terminal */

	if (allow_row_sym) {
		State pps = parentState(ps);

		/* test even row symmetry */
		row_sym_phase_offset = 0;
		for (phase = 0; phase < period; phase++) if (rowOfState(s, phase) != rowOfState(ps, phase)) break;
		if (phase == period) {
			row_symmetry = even;
			return 1;
		}

		/* test odd row symmetry */
		for (phase = 0; phase < period; phase++) if (rowOfState(s, phase) != rowOfState(pps, phase)) break;
		if (phase == period) {
			row_symmetry = odd;
			return 1;
		}

		/* test phase-shifted even row symmetry */
		if ((period & 1) == 0) {
			row_sym_phase_offset = period>>1;
			for (phase = 0; phase < period; phase++)
				if (rowOfState(s, phase) != rowOfState(ps, (phase + row_sym_phase_offset) % period)) break;
			if (phase == period) {
				row_symmetry = even;
				return 1;
			}

			/* test phase-shifted odd row symmetry */
			for (phase = 0; phase < period; phase++)
				if (rowOfState(s, phase) != rowOfState(pps, (phase + row_sym_phase_offset) % period)) break;
			if (phase == period) {
				row_symmetry = odd;
				return 1;
			}
		}
	}

	/* see if we can finish with some rows of stator */
	/* the stator itself will be found later */
	for (i = totalWidth-1; i >= 0; i--) {
		nextTerm = (unsigned short) -1;
		if (term == 0) return 0;
		for (phase = 0; phase < period; phase++) {
			nextTerm &= NEXTTERM(term,rowOfState(s, phase),rowOfState(ps, phase),rowOfState(s, (phase+1)%period),i);
		}
		term = nextTerm;
	}
	nextTerm = (unsigned short) -1;
	switch (symmetry) {
		case odd:
			for (phase = 0; phase < period; phase++)
				nextTerm &= NEXTTERM(term,ODDEXT(rowOfState(s, phase)),ODDEXT(rowOfState(ps, phase)),
										   rowOfState(s, (phase+1)%period)<<1,0);
			if (revTerm[nextTerm] & term) return 1;
			return 0;

		case even:
			for (phase = 0; phase < period; phase++)
				nextTerm &= NEXTTERM(term,EVEXT(rowOfState(s, phase)),EVEXT(rowOfState(ps, phase)),
										   rowOfState(s, (phase+1)%period)<<1,0);
			if (revTerm[nextTerm] & nextTerm) return 1;
			return 0;

		case none:
			for (phase = 0; phase < period; phase++)
				nextTerm &= NEXTTERM(term,rowOfState(s, phase)<<1,rowOfState(ps, phase)<<1,
										   rowOfState(s, (phase+1)%period)<<1,0);
			term = nextTerm;
			nextTerm = (unsigned short) -1;
			for (phase = 0; phase < period; phase++)
				nextTerm &= NEXTTERM(term,rowOfState(s, phase)<<2,rowOfState(ps, phase)<<2,
										   rowOfState(s, (phase+1)%period)<<2,0);
			if (revTerm[nextTerm] & initialTermState) return 1;
			return 0;
	}
#ifdef DEBUG
	printf("is terminal\n");
#endif
	return 1;
}

/* find stator to finish off possible asym stator detected by terminal() */
/* BT(col,i,j) counts min #cells in stator through given col w/last two cols = i, j */
/* PT(col,i,j) gives the preceding column leading to BT(col,i,j) */
short bestTerm[1<<16];
char predTerm[1<<16];
char tcompat[1<<15];
char tcompat3[1<<9];
char stabtab[1<<13];
int fwdBestTerm, backBestTerm;
#define BT(col,i,j) bestTerm[(((col)+2)<<10)|((i)<<5)|(j)]
#define PT(col,i,j) predTerm[(((col)+2)<<10)|((i)<<5)|(j)]
#define tcompatible(i,j,k) tcompat[((i)<<10)|((j)<<5)|(k)]
#define tcomp3(i,j,k) tcompat3[(((i)&7)<<6)|(((j)&7)<<3)|((k)&7)]
int bitCount[32];

static int terminateCols(int backCol, int fwdCol)
{
	int bestCount = 0x7fff;
	int i, j;
	for (i = 0; i < 32; i++) for (j = 0; j < 32; j++) {
		int tot;
		if (BT(backCol,i,j) < 0 || BT(fwdCol,j,i) < 0) continue;
		tot = BT(backCol,i,j) + BT(fwdCol,j,i) - bitCount[i] - bitCount[j];
		if (tot < bestCount) {
			bestCount = tot;
			backBestTerm = i;
			fwdBestTerm = j;
		}
	}
	return (bestCount < 0x7fff);
}

static int stabilizes(int i, int j, int k, State s, int col)
{
	int phase;
	int ijk = ((i&3)<<11) | ((j&3)<<9) | ((k&3)<<7);
	for (phase = 0; phase < period; phase++) {
		Row r = rowOfState(s, phase);
		Row pr = rowOfState(parentState(s), phase);
		Row sr = rowOfState(s, (phase+1)%period);
		if (col >= 0) {
			r >>= col;
			pr >>= col;
			sr >>= col;
		} else switch (symmetry) {
			case odd:
				r = (r<<1)|((r>>1)&1);
				pr = (pr<<1)|((pr>>1)&1);
				sr = (sr<<1)|((sr>>1)&1);
				break;
			case even:
				r = (r<<1)|(r&1);
				pr = (pr<<1)|(pr&1);
				sr = (sr<<1)|(sr&1);
				break;
			case none:
				r <<= -col;
				pr <<= -col;
				sr <<= -col;
				break;
		}
		if (!stabtab[ijk | ((r&7)<<4) | ((pr&7)<<1) | ((sr>>1)&1)])
			return 0;
	}
	return 1;
}

static int terminate(State s) {
	int i,j;
	int col = totalWidth + addlStatorCols;
	int lastCol = -1;


	if (symmetry == none) lastCol = -2;
	if (col > 63) col = 63;
	for (i = 0; i < 32; i++) for (j = 0; j < 32; j++) BT(col,i,j) = -1;
	BT(col,0,0) = 0;	/* empty stator has no cells */
	PT(col,0,0) = 0;  /* and predecessor is also empty */
	while (col > lastCol) {
		int foundAny = 0;
		col--;
		for (i = 0; i < 32; i++) for (j = 0; j < 32; j++) BT(col,i,j) = -1;
		for (i = 0; i < 32; i++) for (j = 0; j < 32; j++) if (BT(col+1,i,j) >= 0) {
			int k;
			for (k = 0; k < 32; k++) {
				if (tcompatible(i,j,k) && BT(col+1,i,j)+bitCount[k] < (BT(col,j,k)&0x7fff) &&
					 	stabilizes(i,j,k,s,col)) {
					BT(col,j,k) = BT(col+1,i,j)+bitCount[k];
					PT(col,j,k) = i;
					foundAny = 1;
				}
			}
		}
		if (!foundAny) return 0;
	}
	switch (symmetry) {
		case even:
			return terminateCols(-1,-1);
		case odd:
			return terminateCols(-1,0);
		case none:
			return terminateCols(totalWidth,-2);
	}
	return 0;	/* never reached */
}

/* initializations for terminal() and terminate() */
static void initTermTabs()
{
	long i, j;
	static int nti[1<<10];

	/* some bit counting stuff. count[] is preshifted, bitCount is bigger and unshifted */
	for (i = 0; i < 8; i++)
	{
		j = (i&1) + ((i>>1)&1) + ((i>>2) & 1);
		count[i] = j << 17;
	}
	for (i = 0; i < 32; i++)
	{
		bitCount[i] = (i&1) + ((i>>1)&1) + ((i>>2) & 1) + ((i>>3)&1) + ((i>>4) & 1);
	}

	/* first pass at compatibility: only low 3 bits of each word */
	for (i = 0; i < 8; i++) for (j = 0; j < 8; j++) {
		int k;
		for (k = 0; k < 8; k++) {
			int count;
			count = 9 - 9*((j>>1)&1);
			count += (i&1) + ((i>>1)&1) + ((i>>2)&1);
			count += (k&1) + ((k>>1)&1) + ((k>>2)&1);
			count += (j&1) + ((j>>2)&1);
			tcomp3(i,j,k) = 0;
			if ((rule&(1<<count))? j&2 : !(j&2)) tcomp3(i,j,k)=1;
		}
	}

	/* full compatibility */
	for (i = 0; i < 32; i++) for (j = 0; j < 32; j++) {
		int k;
		for (k = 0; k < 32; k++) {
			tcompatible(i,j,k) =
				( tcomp3(i,j,k) && tcomp3(i>>1,j>>1,k>>1) && tcomp3(i>>2,j>>2,k>>2) &&
				  tcomp3(i>>3,j>>3,k>>3) && tcomp3(i>>4,j>>4,k>>4) );
		}
	}

	/* stabilization for full terminate() */
	/* format of index: iijjkkrrrppps */
	/* where ijkrrrppp -> s (low bits from ii jj kk respectively) */
	/* and   ijkijkrrr -> j (high and low bits from ii jj kk) */
	for (i = 0; i < (1<<13); i++) {
		stabtab[i] = 0;
		j = 9 - 9*((i>>5)&1);
		j += ((i>>11)&1) + ((i>>9)&1) + ((i>>7)&1);
		j += ((i>>6)&1) + ((i>>4)&1);
		j += ((i>>3)&1) + ((i>>2)&1) + ((i>>1)&1);
		if (rule & (1<<j)? i&1 : !(i&1)) {	/* ijkrrppp -> s ? */
			j = 9 - 9*((i>>9)&1);
			j += ((i>>12)&1) + ((i>>11)&1) + ((i>>10)&1) + ((i>>8)&1);
			j += ((i>>7)&1) + ((i>>6)&1) + ((i>>5)&1) + ((i>>4)&1);
			if (rule & (1<<j)? ((i>>9)&1) : !((i>>9)&1))
				stabtab[i] = 1;
		}
	}

	/* reversal of terminal state */
	for (i = 0; i < (1<<16); i++) {
		revTerm[i] = 0;
		for (j = 0; j < 16; j++) {
			int k = ((j&5)<<1) | ((j&10)>>1);
			if (i & (1<<j)) revTerm[i] |= (1<<k);
			NICE();
		}
	}

	/* single table lookup for terminal state successor */
	/* this is the slow part of the initialization */
	/* to speed it up a little we precompute a lookup table */
	for (i = 0; i < (1<<6); i++) {
		for (j = 0; j < 16; j++) {
			int inner,outer;
			int succ2,count2;
			int succ = i&1;
			int count = (i>>1)&3;
			nti[(j<<6)|i] = 0;
			count += ((i>>3)&1) + ((i>>5)&1);
			count += (j&1) + ((j>>1)&1);
			count += 9 - 9*((i>>4)&1);
			succ2 = (j&1);
			count2 = 9 - 9*succ2 + ((j>>1)&1) + ((j>>2)&1) + ((j>>3)&1);
			count2 += ((i>>3)&1) + ((i>>4)&1) + ((i>>5)&1);
			for (inner = 0; inner < 2; inner++) {
				if ((rule & (1<<(count+inner)))? succ : !succ) for (outer = 0; outer < 2; outer++) {
					if ((rule & (1<<(count2+inner+outer)))? succ2: !succ2)
						nti[(j<<6)|i] |= 1 << (((j&5)<<1) | (outer<<2) | inner);
				}
			}
		}
	}
	for (i = 0; i < (1<<22); i++) {
		NICE();
		nxTerm[i] = 0;
		for (j = 0; j < 16; j++) if (i & (1<<j)) nxTerm[i] |= nti[(i>>16)|(j<<6)];
	}

	/* what states can terminate empty pattern?  and how many cols needed to stabilize?  */
	initialTermState = 1;
	addlStatorCols = 0;
	if (!zeroLotLine) for (;;) {
		unsigned short term = nxTerm[initialTermState];
		if (term == initialTermState) break;
		initialTermState = term;
		addlStatorCols++;
	}
}

/* output stator at asymmetric end of pattern */
static void putStator(int row, int col, int i, int j, int reversed, int skip)
{
#ifdef DEBUG
	printf("BT(%d,%o,%o)=%d; PT=%o\n", col, i, j, BT(col,i,j),  PT(col,i,j));
	if (BT(col,i,j)<0) {
		printf("\nputStator(%d,%d,%o,%o,%d,%d): bad cell count\n", row, col, i, j, reversed, skip);
		exit(0);
	}
#endif
	if (skip <= 0 && reversed) putCell(j,row);
	if (col < totalWidth+addlStatorCols-1)
		putStator(row, col+1, PT(col,i,j), i, reversed, skip-1);
	if (skip <= 0 && !reversed) putCell(j,row);
}

/* found a pattern, output it */
static void success(State s) {
	int i=0, j;
	if (row_symmetry == none && !terminate(s)) return;	/* incomplete success */

	putchar('\n');	/* make initial blank row in output */
	while (parentState(s) != s && s != 0) {
		rows[2*i] = rowOfState(s, 0);
		rows[2*i+1] = rowOfState(s, row_sym_phase_offset);
		i++;
		s = parentState(s);
	}
	j = i;
	while (i-- > 0) putRow(rows[2*i]);
	switch (row_symmetry) {
		case none:
			break;
		case even:
			i = 2;
			while (i < j) putRow(rows[2*(i++)+1]);
			exit(0);
		case odd:
			i = 3;
			while (i < j) putRow(rows[2*(i++)+1]);
			exit(0);
	}
	switch(symmetry) {
		case odd:
			for (i = 0; i < 5; i++) {
				putStator(i,0,fwdBestTerm,backBestTerm,0,1);
				putStator(i,-1,backBestTerm,fwdBestTerm,1,1);
				putchar('\n');
			}
			break;
		case even:
			for (i = 0; i < 5; i++) {
				putStator(i,-1,fwdBestTerm,backBestTerm,0,1);
				putStator(i,-1,backBestTerm,fwdBestTerm,1,1);
				putchar('\n');
			}
			break;
		case none:
			for (i = 0; i < 5; i++) {
				putStator(i,totalWidth,backBestTerm,fwdBestTerm,0,1);
				putStator(i,-2,fwdBestTerm,backBestTerm,1,1);
				putchar('\n');
			}
			break;
	}
	exit(0);
}

/* didn't find a pattern, output anyway */
static void failure() {
	State s = previousState(firstUnprocessedState);
	if (s >= firstState && s < lastState) {
		printf("\nDeepest line found:\n");
		while (parentState(s) != s) {
			putRow(rowOfState(s, 0));
			s = parentState(s);
		}
	} else printf("\nUnable to find current search line.\n");
	exit(0);
}

static void printstatus() {
	State s = previousState(firstUnprocessedState);
	if (s >= firstState && s < lastState) {
		printf("\nCurrent line found:\n");
		while (parentState(s) != s) {
			putRow(rowOfState(s, 0));
			s = parentState(s);
		}
	} else printf("\nUnable to find current search line.\n");
}

/* test whether a state is actually an oscillator of the appropriate period */
static int nontrivial(State s) {
	while (parentState(s) != s) {
		if (aperiodic(s)) return 1;
		s = parentState(s);
	}
	return 0;
}

/* ==================================================================== */
/*  Compute successors of a given state and append them to state space  */
/* ==================================================================== */

/* make new state out of given row indices */
/* stator/rotor check is here for now */
static void makeNewState(State parent) {
	State s = firstFreeState;
	int phase;
	setParentState(s, parent);
	firstFreeState = nextState(s);	/* make sure we're not at end of array */
	for (phase = 0; phase < period; phase++)
		setRowOfState(s, phase, rows[firstRow[phase]+rowIndices[phase]]);
	if (parentState(parent) == parent) {
		int nonzero = 0;
		for (phase = 0; phase < period && !nonzero; phase++)
			if(rowOfState(s, phase)) nonzero++;
		if (!nonzero) {
			firstFreeState = s;	/* zero successor of zero, abort */
			return;
		}
	}
	if (hashing && hash(s)) firstFreeState = s;	/* test if duplicate, and if so abort new state */
}

/* handle subset of rows having a common stator */
static void processGroup(State s)
{
	int phase;
#ifdef DEBUG
		int i;
#endif

	/* set up compatibility information for extensions in adjacent phases */
	for (phase = 0; phase < period; phase++) {
		int i, j;
		int prevPhase = phase - 1;
		if (prevPhase < 0) prevPhase = period - 1;
		rowIndices[phase] = -1;
		for (i = 0; i < nRows[prevPhase]; i++)
			for (j = 0; j < nRows[phase]; j++)
				testCompatible(phase, firstRow[prevPhase]+i, firstRow[phase]+j, s);
	}
	testReachable();

	/* loop through all sequences of compatible extension rows */
	phase = -1;
	for (;;) {
		NICE();
		phase++;
		while (rowIndices[phase] == nRows[phase]-1) {
			rowIndices[phase] = -1;
			phase--;
			if (phase < 0) return;
		}
		rowIndices[phase]++;
#ifdef DEBUG
		printf("state");
		for (i = 0; i <= phase; i++) printf(" %d", rowIndices[i]);
#endif
		if (!reachable(phase, rowIndices[0], rowIndices[phase]))
		{
#ifdef DEBUG
			printf(" unreachable, backtracking\n");
#endif
			phase--;
		} else if (phase > 0 && !compatible(phase, firstRow[phase-1]+rowIndices[phase-1],
												firstRow[phase]+rowIndices[phase])) {
#ifdef DEBUG
			printf(" incompatible, backtracking\n");
#endif
			phase--;
		} else if (phase == period-1) {
			if (compatible(0, firstRow[phase]+rowIndices[phase], firstRow[0]+rowIndices[0])) {
#ifdef DEBUG
				printf(" complete, queueing\n");
#endif
				makeNewState(s);
			}
#ifdef DEBUG
			else printf(" wrap incompatible, continuing\n");
#endif
			phase--;
		}
#ifdef DEBUG
		else printf(" incomplete, continuing\n");
#endif
	}
}

/* qsort subroutine to sort rows by stator type */
/* ties are broken in favor of smaller rotor since qsort appears to be unstable */
static int statorCompare(const void * p, const void * q)
{
	Row pr = *(Row *)p;
	Row qr = *(Row *)q;
	if ((pr&STATMASK) != (qr&STATMASK))
		return (int) ((pr&STATMASK) - (qr&STATMASK));
	return (int) (pr-qr);
}

/* find a single stator group */
int lastRow[MAXPERIOD];
int currentRow[MAXPERIOD];
static void findStatorGroup(State s) {
	int phase;
	Row stator;

	for (phase = 0; phase < period; phase++) {
		/* move past previous stator groups */
		firstRow[phase] += nRows[phase];
		nRows[phase] = 0;

		/* find rows with correct stator */
		if (phase == 0) stator = rows[firstRow[0]] & STATMASK;
		else {
			while (stator > (rows[firstRow[phase]] & STATMASK))
				if (++firstRow[phase] >= lastRow[phase]) { /* ran out of rows in one phase? */
					firstRow[0] = lastRow[0];	/* make process() terminate stator group loop */
#ifdef DEBUG
					printf("Out of rows in phase %d, aborting findStatorGroup and process\n", phase);
#endif
					return;
				}
			if (stator != (rows[firstRow[phase]] & STATMASK)) {
#ifdef DEBUG
				printf("Empty group in phase %d, aborting findStatorGroup\n", phase);
#endif
				return; /* no matches in phase */
			}
		}

		/* here with at least one matching stator in the given phase */
		/* find and count all the rest of the matches */
		while (firstRow[phase]+nRows[phase] < lastRow[phase] &&
				 stator == (rows[firstRow[phase]+nRows[phase]] & STATMASK))
			nRows[phase]++;

#ifdef DEBUG
		printf("Stator group, phase %d: firstRow=%d, nRows=%d\n", phase, firstRow[phase], nRows[phase]);
#endif
	}

	/* stator group all built, process it */
	processGroup(s);
}

/* main entry to enqueue all children of a search node */
static void process(State s)
{
	int phase;
	Row sparkMask = -1L;
#ifdef DEBUG
	printf("processing");
	for (phase = 0; phase < period; phase++)
		printf(" %o", rowOfState(s, phase));
	printf("...\n");
#endif
	NICE();

	/* check if we've finished the search! if so, success doesn't return */
	if (terminal(s) && nontrivial(s)) success(s);

	/* determine how many rows of the state should be treated as sometimes-present sparks */
	if (sparkLevel) {
		int level = 0;
		State p = parentState(parentState(s));	/* discount original two rows */
		if (parentState(p) != p) {
			level = 1;
			if (parentState(parentState(p)) != parentState(p)) level = 2;
		}
		if (sparkLevel > level) {
			if (sparkLevel > level+1) sparkMask =~ EXTIDX(0,-1L,-1L,-1L);
			else sparkMask =~ EXTIDX(0,0,-1L,0);
		}
	}

	/* find representation of set of extensions for each row */
	for (phase = 0; phase < period; phase++) {
		if (phase == 0) firstRow[phase] = 0;
		else firstRow[phase] = firstRow[phase-1]+nRows[phase-1];
		nRows[phase] = 0;
		setupExtensions(rowOfState(s, phase),rowOfState(parentState(s), phase),rowOfState(s, (phase+1)%period),sparkMask);
		listRows(0,phase,totalWidth-1,03);
		if (nRows[phase] == 0) return;	/* no possible extensions in this phase */
	}

	/* simple case: no stators. just process all extension rows together */
	if (STATMASK == 0) {
		processGroup(s);
		return;
	}

	/* have to break things into groups by stator type.  first, sort */
	/* and set up arrays for stator grouping */
	for (phase = 0; phase < period; phase++) {
#ifdef DEBUG
		printf("All rows, phase %d, firstRow=%d, nRows=%d\n", phase, firstRow[phase], nRows[phase]);
#endif
		qsort(rows+firstRow[phase], nRows[phase], sizeof(Row), statorCompare);
		lastRow[phase] = firstRow[phase]+nRows[phase];
		currentRow[phase] = firstRow[phase];
		nRows[phase] = 0;
	}

	while (firstRow[0]+nRows[0] < lastRow[0]) findStatorGroup(s);
}

/* ============================== */
/*  Depth first search algorithm  */
/* ============================== */

#define unused -1

static int depthFirst(State s, int numLevels) {
	State f = firstFreeState;
	NICE();
	if (numLevels == 0) return 1;
	process(s);
	while (f < firstFreeState) {
		State child = previousState(firstFreeState);
		if (depthFirst(child, numLevels - 1)) {
			firstFreeState = f;
			return 1;
		}
		firstFreeState = child;
	}
	firstFreeState = f;
	return 0;
}

static void deepen(int numLevels) {
	State s = firstUnprocessedState;
	while (s < firstFreeState) {
		if (!depthFirst(s, numLevels)) setParentState(s, unused);
		s = nextState(s);
	}
}

/* ============================ */
/*  Garbage-collect full queue  */
/* ============================ */

static int depth(State s) {
	int i = 0;
	while (s != parentState(s)) {
		s = parentState(s);
		i++;
	}
	return i;
}

static void printApprox(long n) {
	n /= period;
	if (n <= 9999) printf("%ld",n);
	else {
		char unit = 'k';
		if (n > 999999) {
			n /= 1000;
			unit = 'M';
		}
		if (n > 99999) printf("%ld%c", n/1000, unit);
		else printf("%ld.%c%c", n/1000, (char)((n%1000)/100)+'0', unit);
	}
}

static void compact() {
	State oldFirstUnproc = firstUnprocessedState;
	State oldFirstFree = firstFreeState;
	State x;
	State y;
	int counter = 0;
	static int lastDepth = 0;
	int frontierDepth = depth(firstUnprocessedState);
	if (frontierDepth > lastDepth) lastDepth = frontierDepth;
	lastDepth++;

	/* initial output and call to iterative deepening code */
	printf("Queue full, depth = %d, ", frontierDepth);
	if (maxDeepen > 0 && rotorWidth > 0 && lastDepth - frontierDepth > maxDeepen) {
		rotorWidth--;
		rightStatorWidth++;
		if (leftStatorWidth > 0 && rotorWidth > 0) {
			leftStatorWidth++;
			rotorWidth--;
		}
		printf("shrinking rotor, ");
		lastDepth = frontierDepth + 1;
	}
	printf("deepening %d, ",lastDepth - frontierDepth);
	printApprox(oldFirstFree - oldFirstUnproc);
	printf("/");
	printApprox(oldFirstFree - firstState);
	fflush(stdout);
	hashing = 0;
	deepen(lastDepth - frontierDepth);	/* do this before outputting arrow */
	hashing = 1;
	printf(" -> ");						/* so user can tell what stage of compaction */
	fflush(stdout);

	/* queue compaction stage 1: mark unused nodes
		we work backwards through the queue, with
		x = next node to be checked for being unused
		y = node that might have x as parent */
	x = previousState(firstUnprocessedState);
	y = previousState(firstFreeState);
	clearHash();
	while (parentState(y) == unused) y = previousState(y);
	do {
		NICE();
		while (parentState(y) != x) {	/* search backwards for y's parent, marking other nodes */
			if (parentState(x) == x) {	/* sanity check */
				fprintf(stderr,"Unable to find parent of y!\n");
				failure();
			}
			setParentState(x, unused);
			x = previousState(x);
			counter++;
		}
		if (parentState(x) == x) break;
		while (parentState(y) == x || parentState(y) == unused)
			y = previousState(y);	/* search backwards for non-child of x */
		x = previousState(x);		/* now done with x, move on to another node */
	} while (parentState(x) != x);

	if (counter) {

		/* queue compaction stage 2: move used nodes forward
			we work forwards through the queue, with
			x = next place to move a node
			y = next node to be moved */

		x = firstState;
		while (parentState(x) != unused) x = nextState(x);
		y = x;
		while (y < firstFreeState) {
			NICE();
			if (parentState(y) != unused) {
				int phase;
				setParentState(x, parentState(y));
				for (phase = 0; phase < period; phase++)
					setRowOfState(x, phase, rowOfState(y, phase));
				x = nextState(x);
			}
			if (y == firstUnprocessedState) firstUnprocessedState = x;
			y = nextState(y);
		}
		firstFreeState = x;

		/* queue compaction stage 3: fix parent pointers
		   at this point, although the actual pointers are all wrong, the pattern
		   of equal/unequal is significant -- if parentState(x) != parentState(previousState(x)),
		   then parentState(x) should be nextState(parentState(previousState(x))).
		   We work forwards through the queue, with x=next node to be fixed,
		   y=old value of parentState(previousState(x)) */

		x = y = firstState;
		x = nextState(x);
		while (x < firstFreeState) {
			NICE();
			if (parentState(x) == y) setParentState(x, parentState(previousState(x)));
			else {
				y = parentState(x);
				setParentState(x, nextState(parentState(previousState(x))));
			}
			(void) hash(x);
			x = nextState(x);
		}
	}

	printApprox(firstFreeState - firstUnprocessedState);
	printf("/");
	printApprox(firstFreeState - firstState);
	printstatus();
	printf("\n");
	fflush(stdout);
}

/* ================================ */
/*  Breadth first search algorithm  */
/* ================================ */

static void breadthFirst(void)
{
	while (firstUnprocessedState != firstFreeState) {
		State s;
		if (firstFreeState >= queueFull) compact();
		s = firstUnprocessedState;
		firstUnprocessedState = nextState(s);
		process(s);
	}
}

/* ================ */
/*  User interface  */
/* ================ */

/* read cmd line from stdin */
static char * readString(char * prompt) {
	static char buf[1024];
	char *s = buf;
	int i = 0;
	char c;

	fputs(prompt, stderr);
	while ((c = getchar()) != '\n')
		if (i++ < 1023) *s++ = c;
	*s++ = '\0';
	s = buf;
	while (*s == ' ' || *s == '\t') s++;	/* skip leading space */
	return s;
}

static void readRule() {
	char * s = readString("Rule: ");
	int shift = 0;
	rule = 0;
	if (*s == '^') readRule();
	else if (*s == '?') {
		printf("Enter the cellular automaton rule, in the form Bxxx/Syyy\n");
		printf("where xxx are digits representing numbers of neighbors that\n");
		printf("cause a cell to be born and yyy represent numbers of neighbors\n");
		printf("that cause a cell to die.  For instance, for Conway's Life\n");
		printf("(the default), the rule would be written B3/S23.\n");
		readRule();
	} else if (*s == '\0') rule = 010014;	/* internal repn of life the universe & everything */
	else for (;;) {
		if (*s >= '0' && *s <= '9') rule |= 1 << (shift + *s - '0');
		else switch(*s) {
			case 'b': case 'B': shift = 9; break;
			case 's': case 'S': shift = 0; break;
			case '/': shift = 9-shift; break;
			case '\0': return;
			default:
				fprintf(stderr,"Unrecognized rule format\n");
				readRule();
				return;
		}
		s++;
	}
}

static Row readRow(int phase) {
	char * s;
	int bit = 0;
	Row row = 0;
	fprintf(stderr,"Phase ");
	if (period > 9 && phase <= 9) fprintf(stderr," ");
	fprintf(stderr,"%d",phase);
	s = readString(": ");
	for (;;) {
		switch(*s) {
			case '.': break;
			case 'o': case 'O': row |= 1<<bit; break;
			case '\0': return row;
			default:
				printf("unexpected character in row input!\n");
				return readRow(phase);
		}
		bit++; s++;
		if (bit > totalWidth) {
			fprintf(stderr,"Too many cells in row!\n");
			return readRow(phase);
		}
	}
}

static int nonInt(char * s) {
	if (*s == '-') s++;
	while (*s != 0)
		if (*s < '0' || *s > '9') return 1;
		else s++;
	return 0;
}

static void helpWidth() {
	printf("Typical oscillators consist of some number of rotor cells (cells that\n");
	printf("actually oscillate) surrounded by other stator cells (still life  patterns\n");
	printf("that stabilize the rotor).  This program allows certain columns to be\n");
	printf("designated as stator cells, which speeds up the search compared to allowing\n");
	printf("all columns to be rotors.  Since you have specified ");
	switch (symmetry) {
		case none:
			printf("no symmetry,\n");
			printf("the columns form three groups: the left stator, the rotor, and the\n");
			printf("right stator.  The width parameters specify how wide to make each group.\n");
			break;

		case even:
			printf("even symmetry,\n");
			printf("the number of stator columns must be equal on each side of the rotor.\n");
			printf("The stator width parameter specifies this number; the number of rotor\n");
			printf("columns is twice the rotor width parameter (because each column appears\n");
			printf("once on each side of the pattern).\n");
			break;

		case odd:
			printf("odd symmetry,\n");
			printf("the number of stator columns must be equal on each side of the rotor.\n");
			printf("The stator width parameter specifies this number; the number of rotor\n");
			printf("columns is twice the rotor width parameter minus one (because each column\n");
			printf("other than the center one appears once on each side of the pattern).\n");
			break;

		default:
			printf("an unknown symmetry mode,\n");
			printf("I can't help you explain how the width parameters are used.\n");
			break;
	}
}

static void readParams() {
	char * s;
	int nInitial;
	enum { rp_rule, rp_period, rp_sym, rp_complete, rp_rotor, rp_left, rp_right, rp_zll,
			 rp_deep, rp_nrows, rp_rows } readParamState = rp_rule;
	fprintf(stderr,"Type ? at any prompt for help, or ^ to return to a previous prompt.\n");
	for (;;) switch (readParamState) {
		case rp_rule:
			readRule();
			readParamState = rp_period;

		case rp_period:
			s = readString("Period: ");
			if (*s == '^') {
				readParamState = rp_rule;
				continue;
			}
			if (*s == '?') {
				printf("Enter the number of generations needed for the pattern\n");
				printf("to repeat its initial configuration.\n");
			}
			period = atoi(s);
			if (nonInt(s) || period < 1 || period >= MAXPERIOD) {
				fprintf(stderr,"Period must be an integer in the range 1..%d\n",MAXPERIOD-1);
				continue;
			}
			readParamState = rp_sym;

		case rp_sym:
			s = readString("Symmetry type (even, odd, none): ");
			switch (*s) {
				case '^': readParamState = rp_period; continue;
				case '?':
					printf("This program is capable of restricting the patterns it seeks\n");
					printf("to those in which each row is symmetric (palindromic).\n");
					printf("This restriction reduces the number of partial patterns that\n");
					printf("must be considered, allowing the program to find patterns\n");
					printf("roughly twice as wide as it could without the symmetry restriction.\n");
					printf("To find patterns in which the rows are symmetric and have even\n");
					printf("length, type E. To find patterns in which the rows are symmetric\n");
					printf("and have odd length, type O. To find asymmetric patterns\n");
					printf("(the default), type N.\n");
					continue;
				case 'e': case 'E': symmetry = even; break;
				case 'o': case 'O': symmetry = odd; break;
				case 'n': case 'N': case '\0': symmetry = none; break;
				default: printf("Unrecognized symmetry option.\n"); continue;
			}
			readParamState = rp_complete;

		case rp_complete:
			s = readString("Allow symmetric completion of patterns (yes, no): ");
			switch (*s) {
				case '^': readParamState = rp_sym; continue;
				case '?':
					printf("If this program detects a symmetric configuration of rows\n");
					printf("in the partial patterns it constructs (for instance, if two\n");
					printf("adjacent rows are the same in each phase) it can immediately\n");
					printf("complete the pattern by repeating the sequence of rows in the\n");
					printf("opposite order, forming a pattern that is symmetric across a\n");
					printf("horizontal axis.  However, this may lead to patterns that are\n");
					printf("roughly twice as long as if they were completed asymmetrically.\n");
					printf("Type Y (the default) to allow symmetric completion, or type N\n");
					printf("to force the search to finish all patterns without early\n");
					printf("symmetry detection.\n");
					continue;
				case 'y': case 'Y': case '\0': allow_row_sym = 1; break;
				case 'n': case 'N': allow_row_sym = 0; break;
				default: printf("Unrecognized completion option.\n"); continue;
			}
			readParamState = rp_rotor;

		case rp_rotor:
			if (period == 1) s = readString("Still life width: ");
			else s = readString("Rotor width: ");
			if (*s == '^') { readParamState = rp_complete; continue; }
			if (*s == '?') { helpWidth(); continue; }
			rotorWidth = atoi(s);
			if (nonInt(s) || rotorWidth <= 0 || rotorWidth > 32) {
				fprintf(stderr,"Width must be an integer in the range 1..32\n");
				continue;
			}
			if (period == 1) readParamState = rp_zll;
			else readParamState = rp_left;
			continue;

		case rp_left:
			if (symmetry == none) {
				s = readString("Left stator width: ");
				if (*s == '^') { readParamState = rp_rotor; continue; }
				if (*s == '?') { helpWidth(); continue; }
				leftStatorWidth = atoi(s);
				if (nonInt(s) || leftStatorWidth < 0 || leftStatorWidth+rotorWidth > 32) {
					fprintf(stderr,"Width must be an integer in the range 0..32\n");
					continue;
				}
			} else leftStatorWidth = 0;
			readParamState = rp_right;

		case rp_right:
			if (symmetry == none) s = readString("Right stator width: ");
			else s = readString("Stator width: ");
			if (*s == '^') {
				if (symmetry == none) readParamState = rp_left;
				else readParamState = rp_rotor;
				continue;
			}
			if (*s == '?') { helpWidth(); continue; }
			rightStatorWidth = atoi(s);
			if (nonInt(s) || rightStatorWidth < 0 || totalWidth > 32) {
				fprintf(stderr,"Width must be an integer in the range 0..32\n");
				continue;
			}
			readParamState = rp_zll;

		case rp_zll:
			s = readString("Allow final stator rows to exceed width limit (yes, no): ");
			switch(*s) {
			case '^':
				if (period == 1) readParamState = rp_rotor;
				else readParamState = rp_right;
				continue;
			case '?':
				printf("The final stator rows of a pattern are found by a different method\n");
				printf("from the main search, that can search for arbitrarily wide patterns\n");
				printf("without significant time penalties.  Normally, to increase the\n");
				printf("chance of a successful search, this stator search is run with a width\n");
				printf("several columns wider than the main search.  Type no here to force\n");
				printf("the whole pattern to stay completely within the given width limits.\n");
				continue;
			case 'n': case 'N': zeroLotLine = 1; break;
			case 'y': case 'Y': case '\0': zeroLotLine = 0; break;
			}
			readParamState = rp_deep;

		case rp_deep:
			s = readString("Maximum deepening amount: ");
			if (*s == '^') { readParamState = rp_zll; continue; }
			if (*s == '?') {
				printf("This program uses a combination of breadth-first and depth-first search\n");
				printf("explained in more detail in http://arXiv.org/abs/cs.AI/0004003.\n");
				printf("When the breadth first queue becomes full, it searches depth-first\n");
				printf("to a level one past the previous depth-first iteration.\n");
				printf("The number of levels of depth first searching provides some indication\n");
				printf("of how the search is progressing; high levels of deepening may\n");
				printf("mean that the difficult part of a pattern has been found and that the\n");
				printf("search is bogging down while trying to finish it off.  In this case,\n");
				printf("it may be appropriate to limit the deepening amount.  If the limit is\n");
				printf("reached, the program attempts to speed the search by restricting\n");
				printf("additional rotor columns to be stators in future rows.  The default\n");
				printf("is to allow arbitrarily large deepening amounts.\n");
				continue;
			}
			maxDeepen = atoi(s);
			if (nonInt(s) || maxDeepen < 0) {
				fprintf(stderr,"Deepening amount must be an integer\n");
				continue;
			}
			readParamState = rp_nrows;

		case rp_nrows:
			s = readString("Number of initially specified rows: ");
			if (*s == '^') { readParamState = rp_deep; continue; }
			if (*s == '?') {
				printf("By default, this program searches for patterns with empty cells\n");
				printf("above them.  This option can be used to specify nonempty cells\n");
				printf("in the rows are above the pattern.  Only the lowest two rows\n");
				printf("can affect the search, so only two rows are allowed to be set.\n");
				printf("\n");
				printf("A negative value -n for this parameter indicates that the program\n");
				printf("should read two rows, but treat the first n of them as sparks that\n");
				printf("might or might not be present near the oscillator.  The oscillator\n");
				printf("itself must run correctly both when the sparks are present and when\n");
				printf("those rows are empty. Further, if the parameter is -2, the oscillator\n");
				printf("should cause the second row of sparks to evolve as described.\n");
				continue;
			}
			nInitial = atoi(s);
			if (nonInt(s)) {
				fprintf(stderr,"Number of initial rows must be an integer\n");
				continue;
			}
			if (nInitial > 2 || nInitial < -2) {
				printf("Must specify 0, 1, or 2 initial rows\n");
				continue;
			}
			if (nInitial < 0) {
				sparkLevel = -nInitial;
				nInitial = 2;
			}
			readParamState = rp_rows;

		case rp_rows:
			makeInitialStates();
			if (!nInitial) return;
			fprintf(stderr,"Specify initial phase of each row; '.'=dead, 'o'=live.\n");
			while (nInitial-- > 0) {
				int phase;
				State s = firstFreeState;
				firstFreeState = nextState(firstFreeState);
				setParentState(s, firstUnprocessedState);
				firstUnprocessedState = s;
				for (phase = 0; phase < period; phase++)
					setRowOfState(s, phase, readRow(phase));
			}
			return;

		default:
			printf("readParams(): unrecognized state!\n");
			exit(0);
	}


}

/* ============ */
/*  Main entry  */
/* ============ */

int main(void)
{
	printf("ofind 0.9, D. Eppstein, 14 August 2000\n");
	initHash();
	readParams();
	printf("Initializing... "); fflush(stdout);
	makeDownShifts();
	makeExtTab();
	initTermTabs();
	if (tcompatible(0,2,0)) printf("bad tcompat!\n");
	printf("Searching...\n"); fflush(stdout);
	fflush(stdout);
	breadthFirst();
	printf("No patterns found\n");
	failure();
	return 0;
}

