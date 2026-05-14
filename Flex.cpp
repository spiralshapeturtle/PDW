// Flex.cpp
//
// This file uses the following functions:
//
//	  FLEX::FLEX()
//	  FLEX::~FLEX()
//	  int FLEX::xsumchk(long int l)
//	  void FLEX::show_addr(long int l)
//	  void FLEX::show_addr(long int l1, long int l2)
//	  void FLEX::show_phase_speed(int vt)
//	  void FLEX::showframe(int asa, int vsa)
//	  void FLEX::showblock(int blknum)
//	  void FLEX::showword(int wordnum)
//	  void FLEX::showwordhex(int wordnum)
//	  void frame_flex(char gin)


#ifndef STRICT
#define STRICT 1
#endif

#include <windows.h>

#include "headers\pdw.h"
#include "headers\sound_in.h"
#include "headers\misc.h"
#include "headers\helper_funcs.h"
#include "headers\initapp.h"
#include "utils\debug.h"
#include "utils\debuglog.h"

#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))

#define MODE_SECURE				0
#define MODE_SHORT_INSTRUCTION	1
#define MODE_SH_TONE			2
#define MODE_STNUM				3
#define MODE_SFNUM				4
#define MODE_ALPHA				5
#define MODE_BINARY				6
#define MODE_NUNUM				7

//#define SYNC0	0x870C
#define SYNC1	0xA6C6
#define SYNC2	0xAAAA
//#define SYNC3	0x78F3

#define EOT1	0xAAAA
#define EOT2	0xFFFF

// FLEX message fragment reassembly — MC68175 spec-based K/F/C classification.
// Alpha message header word (standard FLEX): frag = bits 11-12 (2-bit), cont = bit 10 (1-bit).
//   K (complete):  frag==3 && cont==0 — standalone; show directly
//   F (first):     frag==3 && cont==1 — first fragment; start new chain
//   F (cont):      frag!=3 && cont==1 — continuation; only valid if prior F=11 chain exists
//   C (last):      frag!=3 && cont==0 — last fragment; assemble chain + show
// frag==3 (F=11) is the ONLY valid chain start. Continuation arriving without a prior
// F=11 chain = mid-stream orphan (header lost in transit) — discard silently.
// After F=11, subsequent fragments must follow the strict 0->1->2 modulo-3 cycle.
#define FLEX_FRAG_COMPLETE  3	// only this constant is still used (C-type check)

#define FLEX_MAX_FRAG_SLOTS   16
#define FLEX_FRAG_TIMEOUT_MS  120000u   // abandon incomplete chain after 2 minutes

struct FlexFragSlot {
	bool          active;
	long          capcode;
	ULONGLONG     timestamp_ms;
	unsigned char text [MAX_STR_LEN];
	BYTE          color[MAX_STR_LEN];
	int           textLen;
	int           nextExpectedFrag;  // 0, 1, or 2: next F-value in the modulo-3 cycle
};

static FlexFragSlot g_flexFragSlots[FLEX_MAX_FRAG_SLOTS];

// These globals live in Misc.cpp; used by fragment helpers below.
extern BYTE message_color[];
extern int  iMessageIndex;
extern int  nCount_Fragments;

// Set before calling ShowMessage() to signal assembly result to ShowMessage().
// g_flexAssembled: message was reconstructed from multiple fragments.
// g_flexOrphanType: 1 = F-type orphan (first/middle, no slot), 2 = C-type orphan (last, no prior chain).
bool g_flexAssembled  = false;
int  g_flexOrphanType = 0;

// flex_reset() can't reach the function-scope statics in frame_flex (slr, bct,
// hbit, cy, fr) directly. Setting this flag asks frame_flex to wipe them on
// its next entry — that way mode switches don't leave the 64-bit shift
// register and frame counters drifting on data from a previous transmission.
static volatile bool g_flexFrameFlexResetRequested = false;

// Forward-declared here; defined together with display_cfstatus at the bottom
// of the file. Cleared when the BCH stage rejects the cycle/frame info word,
// preventing the 99/999 sentinel from leaking into Check4_MissedGroupcalls.
extern bool bCurrentFrameValid;


int flex_blk = 0;
int flex_bc  = 0;

extern PROFILE Profile;			// profile information

extern PaneStruct Pane1;
extern PaneStruct Pane2;

long int capcode;
int FlexTempAddress;			// PH: Set to corresponding groupaddress (0-15)
int FLEX_9=0;					// PH: Set if receiving 9-digit capcodes

bool bEmpty_Frame;				// PH: Set if FLEX-Frame=EMTPY / ERMES-Batch=0

bool bFLEX_groupmessage;		// PH: Set if receiving a groupmessage (2029568-2029583)
bool bFLEX_Frame_contains_SI;	// PH: Set if this frame contains Short instructions
bool bFlexTIME_detected=false;	// PH: Set if FlexTIME is detected
bool bFlexTIME_not_used=false;	// PH: Set if FlexTIME is not used on this network

SYSTEMTIME recFlexTime, recTmpTime;
FILE *pFlexTIME = NULL;

extern int flex_timer;
extern int iCurrentFrame;		// current flex cycle
extern int iCurrentCycle;		// current flex frame

extern char ob[32];

extern bool bFlexActive, bReflex;

char vtype[8][9]={"SECURE ", " INSTR ", "SH/TONE", " StNUM ",
				  " SfNUM ", " ALPHA ", "BINARY ", " NuNUM "};

int flex_speed = STAT_FLEX1600;
int g_sps=1600;
int g_sps2=1600;
int level=2;
int syncs[8] = { 0x870C, 0x7B18, 0xB068, 0xDEA0, 0, 0, 0, 0x4C7C };
//int syncs[8] = { 0x870C, 0x7B18, 0xB068, 0xDEA0, 0x22B4, 0xE9C4, 0x4C7C, 0x34DF };

char phase;


FLEX::FLEX()
{
}


FLEX::~FLEX()
{
}

// --- Fragment buffer helpers ---

static void frag_expire(void)
{
	ULONGLONG now = GetTickCount64();
	for (int i = 0; i < FLEX_MAX_FRAG_SLOTS; i++)
		if (g_flexFragSlots[i].active &&
			(now - g_flexFragSlots[i].timestamp_ms) > FLEX_FRAG_TIMEOUT_MS)
		{
			ULONGLONG elapsedMs = now - g_flexFragSlots[i].timestamp_ms;
			DebugLog("[FRAG] TIMEOUT slot=%d  capcode=%07li  elapsed=%llums  discarded=%d chars  expectedFrag=%d",
				i, g_flexFragSlots[i].capcode, elapsedMs,
				g_flexFragSlots[i].textLen, g_flexFragSlots[i].nextExpectedFrag);
			g_flexFragSlots[i].active = false;
		}
}

static int frag_find(long cc)
{
	for (int i = 0; i < FLEX_MAX_FRAG_SLOTS; i++)
		if (g_flexFragSlots[i].active && g_flexFragSlots[i].capcode == cc)
			return i;
	return -1;
}

static int frag_alloc(long cc)
{
	frag_expire();
	// reuse existing slot for same capcode (F=11 retransmit/restart: flush + start fresh)
	int slot = frag_find(cc);
	if (slot >= 0) {
		g_flexFragSlots[slot].textLen            = 0;
		g_flexFragSlots[slot].nextExpectedFrag   = 0;
		g_flexFragSlots[slot].timestamp_ms       = GetTickCount64();
		return slot;
	}
	for (int i = 0; i < FLEX_MAX_FRAG_SLOTS; i++) {
		if (!g_flexFragSlots[i].active) {
			g_flexFragSlots[i].active             = true;
			g_flexFragSlots[i].capcode            = cc;
			g_flexFragSlots[i].textLen            = 0;
			g_flexFragSlots[i].nextExpectedFrag   = 0;
			g_flexFragSlots[i].timestamp_ms       = GetTickCount64();
			return i;
		}
	}
	return -1; // all slots occupied
}

// Append current message_buffer content to slot and reset iMessageIndex.
static void frag_save(int slot)
{
	FlexFragSlot &s = g_flexFragSlots[slot];
	int space       = MAX_STR_LEN - 1 - s.textLen;
	int tocopy      = min(iMessageIndex, space);
	if (tocopy > 0) {
		memcpy(s.text  + s.textLen, message_buffer, tocopy);
		memcpy(s.color + s.textLen, message_color,  tocopy);
		s.textLen += tocopy;
	}
	iMessageIndex = 0;
}

// Prepend the slot's accumulated text to the current message_buffer and free the slot.
// After this call message_buffer holds the fully assembled message.
static void frag_assemble(int slot)
{
	FlexFragSlot &s = g_flexFragSlots[slot];
	int space       = MAX_STR_LEN - 1 - s.textLen;
	int tocopy      = min(iMessageIndex, space);
	if (s.textLen > 0) {
		memmove(message_buffer + s.textLen, message_buffer, tocopy);
		memmove(message_color  + s.textLen, message_color,  tocopy);
		memcpy (message_buffer, s.text,  s.textLen);
		memcpy (message_color,  s.color, s.textLen);
		iMessageIndex = s.textLen + tocopy;
	}
	message_buffer[iMessageIndex] = 0;	// ShowMessage() loops on != 0; ensure termination
	s.active = false;
}

// Public helper for the SI/groupcall counter logic in Misc.cpp.
// Returns true if a fragment chain is currently in progress for the given capcode.
// Used by Check4_MissedGroupcalls() to suppress false X++ during multi-fragment
// group messages: if the group call is mid-chain, it is NOT a missed call.
bool flex_has_pending_fragment(long capcode)
{
	return frag_find(capcode) >= 0;
}

// Reset routine called when changing data mode or if
// switching between soundcard & serial port input.
void flex_reset(void)
{
	extern FLEX phase_A, phase_B, phase_C, phase_D;
	flex_blk = 0;
	flex_bc = 0;
	flex_timer = 0;
	bReflex = false;
	bFlexActive = false;
	memset(g_flexFragSlots, 0, sizeof(g_flexFragSlots));
	// Clear per-phase scratch arrays so the first frame after a mode switch
	// doesn't start on stale bits from the previous transmission. Without
	// this, showblock() can sample garbage in the first BIW until the BCH
	// stage filters it out.
	memset(phase_A.block, 0, sizeof(phase_A.block));
	memset(phase_A.frame, 0, sizeof(phase_A.frame));
	memset(phase_B.block, 0, sizeof(phase_B.block));
	memset(phase_B.frame, 0, sizeof(phase_B.frame));
	memset(phase_C.block, 0, sizeof(phase_C.block));
	memset(phase_C.frame, 0, sizeof(phase_C.frame));
	memset(phase_D.block, 0, sizeof(phase_D.block));
	memset(phase_D.frame, 0, sizeof(phase_D.frame));
	// Ask frame_flex to wipe its function-scope statics on the next call.
	g_flexFrameFlexResetRequested = true;
}

// checksum check for BIW and vector type words
// returns: 0 if word passes test; 1 if test failed
int FLEX::xsumchk(long int l)
{
	// was word already marked as bad?
	if (l > 0x3fffffl) return(1);

	// 4 bit checksum is made by summing up remaining part of word
	// in 4 bit increments, and taking the 4 lsb and complementing them.
	// Therefore: if we add up the whole word in 4 bit chunks the 4 lsb
	// bits had better come out to be 0x0f

	int xs = (int) (l	 & 0x0f);
	xs += (int) ((l>> 4) & 0x0f);
	xs += (int) ((l>> 8) & 0x0f);
	xs += (int) ((l>>12) & 0x0f);
	xs += (int) ((l>>16) & 0x0f);
	xs += (int) ((l>>20) & 0x01);

	xs = xs & 0x0f;

	if (xs == 0x0f)
	{
		CountBiterrors(0);
		return(0);
	}
	else
	{
		CountBiterrors(1);
		return(1);
	}
}


// converts a short flex address to a CAPCODE; shows it on screen
void FLEX::show_address(long int l, long int l2, bool bLongAddress)
{
	int len = bLongAddress ? FILTER_CAPCODE_LEN : FILTER_CAPCODE_LEN-2;

	if (!bLongAddress)
	{
		capcode = (l & 0x1fffffl) - 32768l;

		if (FLEX_9) FLEX_9--;
	}
	else
	{	
		// to get capcode: take second word, invert it...
		capcode = (l2 & 0x1fffffl) ^ 0x1fffffl;

		// multiply by 32768
		capcode = capcode << 15;

		// add in 2068480 and first word
		// NOTE : in the patent for FLEX, the number given was 2067456...
		//			 which is apparently not correct
		capcode = capcode + 2068480l + (l & 0x1fffffl);

		if (FLEX_9 < 91) FLEX_9 += 10;
	}
	
	// capcode is bad if it was derived from a word with uncorrectable
	// errors or if it is less than zero

	if ((l > 0x3fffffl) || (l2 > 0x3fffffl) || (capcode < 0))
	{
		strcpy(Current_MSG[MSG_CAPCODE], bLongAddress ? "?????????" : "???????");

		capcode=9999999;

		CountBiterrors(5);
	}
	else // OK here!
	{
		sprintf(Current_MSG[MSG_CAPCODE], bLongAddress ? "%09li" : "%07li", capcode);

		CountBiterrors(0);
	}

	if (Profile.convert_si && (capcode >= 2029568) && (capcode <= 2029583))
	{
		 bFLEX_groupmessage=true;
	}
	else bFLEX_groupmessage=false;

	/* Show Capcode */

	messageitems_colors[1] = COLOR_ADDRESS;

	/* Show Time/Date */

	Get_Date_Time();
	strcpy(Current_MSG[MSG_TIME], szCurrentTime);
	strcpy(Current_MSG[MSG_DATE], szCurrentDate);
	messageitems_colors[2] = COLOR_TIMESTAMP;
	messageitems_colors[3] = COLOR_TIMESTAMP;
}


/**************************/

// Return current bit rate based on flex_speed.
void FLEX::show_phase_speed(int vt)
{
	int v;

	switch(flex_speed)	// Add Bit Rate
	{
		default:
		case STAT_FLEX1600: v=1600;
		break;

		case STAT_FLEX3200: v=3200;
		break;

		case STAT_FLEX6400: v=6400;
		break;
	}

	/* Show FLEX-# */

	messageitems_colors[4] = COLOR_MODETYPEBIT;
	sprintf(Current_MSG[MSG_MODE], "FLEX-%c", phase);

	/* Show Type */

	messageitems_colors[5] = COLOR_MODETYPEBIT;

	if (vt == MODE_SHORT_INSTRUCTION && Profile.convert_si)
	{
		strcpy(Current_MSG[MSG_TYPE], " GROUP ");	// PH: Add "GROUP" in stead of "INSTR"
	}
	else
	{
		strcpy(Current_MSG[MSG_TYPE], vtype[vt]);	// Add flex format.
	}

	/* Show Bitrate */

	messageitems_colors[6] = COLOR_MODETYPEBIT;
	sprintf(Current_MSG[MSG_BITRATE], "%d", v);
}

/*

void FLEX::CheckFlexTime(void)
{
	SYSTEMTIME recSystemTime;
	time_t lPCTime, lFlexTime;

	GetLocalTime(&recSystemTime);
	lPCTime = recSystemTime.wHour * 3600 + recSystemTime.wMinute * 60 + recSystemTime.wSecond;
	lFlexTime = recFlexTime.wHour * 3600 + recFlexTime.wMinute * 60 + recFlexTime.wSecond;

	lPCTime = abs(lPCTime - lFlexTime);

	if(recFlexTime.wYear != recSystemTime.wYear || recFlexTime.wMonth != recSystemTime.wMonth || recFlexTime.wDay != recSystemTime.wDay || lPCTime > 2) 
	{
		OUTPUTDEBUGMSG((("TIME OUT OF SYNC! (%d seconds) \n"), lPCTime));		
		SetLocalTime(&recFlexTime);
		if (pFlexTIME) fwrite(" Systemtime has been corrected!", 31, 1, pFlexTIME);
	}
}

*/

void FLEX::FlexTIME()
{
	if (strstr(szPath, "DEBUG")) return;

	int i;
	char temp[MAX_PATH];
	char szFlexTIME[128];

	int seconds = (int)((iCurrentFrame & 0x1f) * 1.875f);

	static int FLEX_time=0, FLEX_date=0, count=0;

	bool bTime = false, bDate = false;

//	OUTPUTDEBUGMSG((("Frame[0] = 0x%08X\n"), frame[0]));		
//	OUTPUTDEBUGMSG((("Priority addresses %d\n"), (frame[0] >> 4) & 0xF));		
//	OUTPUTDEBUGMSG((("End Block %d\n"), (frame[0] >> 8) & 0x3));		
//	OUTPUTDEBUGMSG((("Vector %d\n"), (frame[0] >> 10) & 0x7));		
//	OUTPUTDEBUGMSG((("Frame Id %d\n"), (frame[0] >> 16) & 0x7));		

	for (i=0; i<=((frame[0] >> 8) & 0x03); i++)
	{
		if(xsumchk(frame[i]) != 0)
		{
//			OUTPUTDEBUGMSG((("CRC error in BIW[%d]! (0x%08X)\n"), i, frame[i]));
			return;
		}
		if(i)
		{
			switch((frame[i] >> 4) & 0x07)
			{
				case 0:
//					OUTPUTDEBUGMSG((("frame[i]: Type == SSID/Local ID\xbbs (i8-i0)(512) & Coverage Zones (c4-c0)(32)\n")));		
					break;
				case 1:
					frame[i] >>= 7;
					recFlexTime.wYear = (frame[i] & 0x1F) + 1994;
					frame[i] >>= 5;
					recFlexTime.wDay = frame[i] & 0x1F;
					frame[i] >>= 5;
					recFlexTime.wMonth = (frame[i] & 0xF);
					bDate = true;
					FLEX_date=1;
//					OUTPUTDEBUGMSG((("BIW DATE: %d-%d-%d\n"), recFlexTime.wDay, recFlexTime.wMonth, recFlexTime.wYear));		
					break;
				case 2:
					frame[i] >>= 7;
					recFlexTime.wHour = frame[i] & 0x1F;
					frame[i] >>= 5;
					recFlexTime.wMinute = frame[i] & 0x3F;
					frame[i] >>= 6;
					recFlexTime.wSecond = seconds;
					bTime = true;
					FLEX_time=1;
//					OUTPUTDEBUGMSG((("BIW TIME: %02d:%02d:%02d\n"), recFlexTime.wHour, recFlexTime.wMinute, recFlexTime.wSecond));
					break;
				case 5:
//					OUTPUTDEBUGMSG((("frame[i]: Type == System Information (I9-I0. A3-A0) - related to NID roaming\n")));		
					break;
				case 7:
//					OUTPUTDEBUGMSG((("frame[i]: Type == Country Code & Traffic Management Flags (c9-c0, T3-T0)\n")));		
					break;
				case 6:
				case 3:
				case 4:
//					OUTPUTDEBUGMSG((("frame[i]: Type == Reserved\n")));		
					break;
			}
		}
	}

	if (FLEX_time && FLEX_date && !bFlexTIME_detected) bFlexTIME_detected = true;

	if (iCurrentFrame == 0)
	{
		count++;

		if (count == 15 && !bFlexTIME_detected) bFlexTIME_not_used = true;
		else if (Profile.FlexTIME)
		{
			if (bTime || bDate)
			{
				if (bTime)
				{
					GetLocalTime(&recTmpTime);
					recTmpTime.wHour   = recFlexTime.wHour;
					recTmpTime.wMinute = recFlexTime.wMinute;
					recTmpTime.wSecond = recFlexTime.wSecond;
					recTmpTime.wMilliseconds = 0;
					SetLocalTime(&recTmpTime);
				}

				if (bDate)
				{
					GetLocalTime(&recTmpTime);
					recTmpTime.wDay   = recFlexTime.wDay;
					recTmpTime.wMonth = recFlexTime.wMonth;
					recTmpTime.wYear  = recFlexTime.wYear;
					SetLocalTime(&recTmpTime);
				}
			}
		}
	}
}

// This routine is called when a complete phase of information is collected.
// First, the BIW is used to determine the length of the address and vector field blocks.
// Each address field is then processed according to the information in the vector field.
void FLEX::showframe(int asa, int vsa)
{
	int vb, vt, tt, w1, w2, j, k, l, m, n=0, i, c=0;
	long int cc, cc2, cc3;
	bool bLongAddress=false, bXsumError=false, bFragmentBuffered=false;

	int iFragmentNumber, iContFlag, iAssignedFrame;

	extern unsigned long hourly_stat[NUM_STAT][2];
	extern unsigned long hourly_char[NUM_STAT][2];
	extern unsigned long daily_stat[NUM_STAT][2];
	extern unsigned long daily_char[NUM_STAT][2];
	
	char szTemp[128];

	extern char szWindowText[6][1000];

	FlexTempAddress = -1;		// PH: Current temporary address(bit)

	frag_expire();			// clean up timed-out fragment chains each frame

	DebugLogNotifyFrameChange(iCurrentFrame);
	DebugLog("[showframe] %02d/%03d  asa=%d vsa=%d  addresses=%d",
		DebugLogGetCycle(), iCurrentFrame, asa, vsa, vsa - asa);

	if (xsumchk(frame[0]) == 0)			// make sure we start out with valid BIW
	{
		// Defence in depth: even with the BIW checksum passing, a BCH miscorrection
		// could in principle produce asa/vsa values outside the FLEX spec window
		// (1 <= asa <= vsa <= 88). frame[] has 200 entries so we're not at risk of
		// OOB at the spec limits, but bail out if asa/vsa indices are obviously
		// inconsistent so the address-loop doesn't iterate over vector words.
		if (asa < 1 || asa > vsa || vsa > 88) return;

		for (j=asa; j<vsa; j++, c=0, bLongAddress=false, bXsumError=false, bFragmentBuffered=false) // run through whole address field
		{
			cc2 = frame[j] & 0x1fffffl;	// Check if this can be the low part of a long address

			// check for long addresses (bLongAddress indicates long address)
			if (cc2 < 0x008001l) bLongAddress=true;
			else if ((cc2 > 0x1e0000l) && (cc2 < 0x1f0001l)) bLongAddress=true;
			else if (cc2 > 0x1f7FFEl) bLongAddress=true;

			vb = vsa + j - asa;	// this is the vector word number associated with the address word j
			vt = (frame[vb] >> 4) & 0x07;	// get message vector type

			if (xsumchk(frame[vb]) != 0)
			{
				// Long addresses occupy two address slots; if we skip here
				// without advancing j, the next iteration mis-parses the
				// second half as a new short capcode.
				if (bLongAddress) j++;
				continue; 	// screwed up vector fields are not processed
			}
			if (Profile.FlexGroupMode && bLongAddress)
			{
				j++;
				continue; 	// Don't process long addresses if FlexGroupMode
			}
			strcpy(szWindowText[4], "");

			switch(vt)
			{
				case MODE_ALPHA:
				case MODE_SECURE:

				// FIX [Berichtdecodering]: j<vsa<=88, dus j+1<=88<200 — frame[j+1] altijd binnen bounds.
				show_address(frame[j], frame[j+1], bLongAddress);	// show address
				show_phase_speed(vt);

				// get start and stop word numbers
				w1 = frame[vb] >> 7;
				w2 = w1 >> 7;
				w1 = w1 & 0x7f;
				w2 = (w2 & 0x7f) + w1 - 1;
				if (w2 > 199) w2 = 199;  // BCH miscorrection can produce w2 up to 253; clamp to frame[] bounds
				if (w1 > w2)  { if (bLongAddress) j++; continue; }

				// Standard FLEX (North American) word layout: F=bits 11-12, C=bit 10.
				// (RCR STD-43A / FLEX-TD differs: it adds a 10-bit K checksum that shifts
				//  the fields to F=bits 8-9, C=bit 7 — but NL/EU networks use standard FLEX.)
				// K/F/C: K=frag==3&&cont==0, F=cont==1, C=cont==0&&frag!=3
				if (!bLongAddress)
				{
					iFragmentNumber = (int)(frame[w1] >> 11) & 0x03;
					iContFlag       = (int)(frame[w1] >> 10) & 0x01;
					w1++;
				}
				else
				{
					// FIX [Berichtdecodering]: expliciete boundscheck op frame[vb+1].
					// Invariant: vb=vsa+j-asa<=174; frame[175] is safe, maar toekomstige
					// wijzigingen aan de vsa-bound zouden dit ongeldig maken.
					if (vb + 1 >= 200) { j++; continue; }
					iFragmentNumber = (int)(frame[vb+1] >> 11) & 0x03;
					iContFlag       = (int)(frame[vb+1] >> 10) & 0x01;
					w2--;
				}

				for (k=w1; k<=w2; k++)				// dump all message characters onto screen
				{
					if (frame[k] > 0x3fffffl) display_color(&Pane1, COLOR_BITERRORS);
					else display_color(&Pane1, COLOR_MESSAGE);

					// skip over header info (depends on fragment number)
					if ((k > w1) || (iFragmentNumber != 0x03))
					{
						c = (int) frame[k] & 0x7fl;
						if (c != 0x03)
						{
							display_show_char(&Pane1, c);
							hourly_char[flex_speed][STAT_ALPHA]++;
							daily_char [flex_speed][STAT_ALPHA]++;
						}
					}

					cc = (long) frame[k] >> 7;
					c = (int) cc & 0x7fl;

					if (c != 0x03)
					{
						display_show_char(&Pane1, c);
						hourly_char[flex_speed][STAT_ALPHA]++;
						daily_char [flex_speed][STAT_ALPHA]++;
					}

					cc = (long) frame[k] >> 14;
					c = (int) cc & 0x7fl;

					if (c != 0x03)
					{
						display_show_char(&Pane1, c);
						hourly_char[flex_speed][STAT_ALPHA]++;
						daily_char [flex_speed][STAT_ALPHA]++;
					}
				}

				// Fragment reassembly using multimon-ng K/F/C classification:
				// F (cont==1): more follows — buffer; C (cont==0, frag!=3): last — assemble;
				// K (cont==0, frag==3): standalone complete — fall through to ShowMessage().
				if (capcode != 9999999)
				{
					// Capture text before any frag_save() resets iMessageIndex.
					// Buffer sized to hold a full message (MAX_STR_LEN) so the [ALPHA] log line
					// is never truncated; downstream comparison tools rely on the complete text.
					static char szDbgText[MAX_STR_LEN + 1]; int dbgLen = min(iMessageIndex, MAX_STR_LEN);  // FIX [L4]: static voorkomt 5 KB stack-allocatie per aanroep
					memcpy(szDbgText, message_buffer, dbgLen); szDbgText[dbgLen] = '\0';

					if (iContFlag == 1)
					{
						if (iFragmentNumber == FLEX_FRAG_COMPLETE)
						{
							// F=11, C=1: first fragment of a multi-part chain.
							// Capture pre-flush state for the RESTART log entry.
							int existingSlot = frag_find(capcode);
							int discardedLen = (existingSlot >= 0) ? g_flexFragSlots[existingSlot].textLen : 0;
							int discardedExp = (existingSlot >= 0) ? g_flexFragSlots[existingSlot].nextExpectedFrag : -1;
							// frag_alloc() resets an existing slot (retransmit/restart) or claims a free one.
							int slot = frag_alloc(capcode);
							if (slot >= 0) {
								frag_save(slot);
								bFragmentBuffered = true;
								if (existingSlot >= 0)
									DebugLog("[FRAG] %02d/%03d  capcode=%07li  F=11 RESTART slot=%d  discarded=%d chars (was at expected=%d)  partial=\"%s\"",
										DebugLogGetCycle(), iCurrentFrame, capcode, slot,
										discardedLen, discardedExp, szDbgText);
								else
									DebugLog("[FRAG] %02d/%03d  capcode=%07li  F-type first slot=%d  partial=\"%s\"",
										DebugLogGetCycle(), iCurrentFrame, capcode, slot, szDbgText);
							}
							else {
								g_flexOrphanType = 1; // all slots full; show this first fragment alone with label
								DebugLog("[FRAG] %02d/%03d  capcode=%07li  F-type ORPHAN (all 16 slots full)  partial=\"%s\"",
									DebugLogGetCycle(), iCurrentFrame, capcode, szDbgText);
							}
						}
						else
						{
							// F!=11, C=1: continuation — only valid if a prior F=11 chain exists for this capcode.
							int slot = frag_find(capcode);
							if (slot >= 0) {
								if (iFragmentNumber == g_flexFragSlots[slot].nextExpectedFrag) {
									int prevLen = g_flexFragSlots[slot].textLen;
									g_flexFragSlots[slot].nextExpectedFrag = (iFragmentNumber + 1) % 3;
									g_flexFragSlots[slot].timestamp_ms     = GetTickCount64();
									frag_save(slot);
									bFragmentBuffered = true;
									DebugLog("[FRAG] %02d/%03d  capcode=%07li  F-type continuation slot=%d  frag=%d (mod3 OK)  chain=%d->%d chars  partial=\"%s\"",
										DebugLogGetCycle(), iCurrentFrame, capcode, slot,
										iFragmentNumber, prevLen, g_flexFragSlots[slot].textLen, szDbgText);
								}
								else {
									// Row 5: out-of-sequence missing fragment — abort chain, discard silently.
									DebugLog("[FRAG] %02d/%03d  capcode=%07li  F-type SEQUENCE ERROR slot=%d  got=%d expected=%d  chain=%d chars before abort",
										DebugLogGetCycle(), iCurrentFrame, capcode, slot,
										iFragmentNumber, g_flexFragSlots[slot].nextExpectedFrag,
										g_flexFragSlots[slot].textLen);
									g_flexFragSlots[slot].active = false;
									bFragmentBuffered = true; // discard this fragment silently
								}
							}
							else {
								// No prior F=11 chain — mid-stream orphan (header lost in transit).
								DebugLog("[FRAG] %02d/%03d  capcode=%07li  F-type mid-stream ORPHAN (no chain, frag=%d)",
									DebugLogGetCycle(), iCurrentFrame, capcode, iFragmentNumber);
								bFragmentBuffered = true; // discard silently
							}
						}
					}
					else if (iFragmentNumber != FLEX_FRAG_COMPLETE)
					{
						// C-type (cont==0, frag!=3): last fragment — assemble chain.
						int slot = frag_find(capcode);
						if (slot >= 0) {
							if (iFragmentNumber == g_flexFragSlots[slot].nextExpectedFrag) {
								int prevLen = g_flexFragSlots[slot].textLen;
								frag_assemble(slot);
								nCount_Fragments++;
								g_flexAssembled = true; // signal ShowMessage() to show assembled indicator
								int dbgLen2 = min(iMessageIndex, MAX_STR_LEN);
								memcpy(szDbgText, message_buffer, dbgLen2); szDbgText[dbgLen2] = '\0';
								DebugLog("[FRAG] %02d/%03d  capcode=%07li  C-type assembled slot=%d  frag=%d (mod3 OK)  prefix=%d + last=%d = total %d chars  \"%s\"",
									DebugLogGetCycle(), iCurrentFrame, capcode, slot,
									iFragmentNumber, prevLen, iMessageIndex - prevLen, iMessageIndex, szDbgText);
								// Emit a standard [ALPHA] line with K-type tag so downstream tools
								// see fragmented and non-fragmented messages in identical format.
								DebugLog("[ALPHA] %02d/%03d  capcode=%07li  K-type  \"%s\"",
									DebugLogGetCycle(), iCurrentFrame, capcode, szDbgText);
							}
							else {
								// Row 8: out-of-sequence last fragment — abort chain, discard silently (consistent with Row 5).
								DebugLog("[FRAG] %02d/%03d  capcode=%07li  C-type SEQUENCE ERROR slot=%d  got=%d expected=%d  chain=%d chars discarded",
									DebugLogGetCycle(), iCurrentFrame, capcode, slot,
									iFragmentNumber, g_flexFragSlots[slot].nextExpectedFrag,
									g_flexFragSlots[slot].textLen);
								g_flexFragSlots[slot].active = false;
								bFragmentBuffered = true; // discard silently
							}
						}
						else {
							// Row 9: orphan last fragment (no prior chain) — discard silently (Rule 5).
							DebugLog("[FRAG] %02d/%03d  capcode=%07li  C-type mid-stream ORPHAN (no chain, frag=%d)",
								DebugLogGetCycle(), iCurrentFrame, capcode, iFragmentNumber);
							bFragmentBuffered = true; // discard silently
						}
					}
					else
					{
						// Row 1: K-type (frag==3, cont==0) standalone — fall through to ShowMessage().
						// Existing fragmentation slots for this capcode are intentionally left untouched.
						int openSlot = frag_find(capcode);
						if (openSlot >= 0)
							DebugLog("[ALPHA] %02d/%03d  capcode=%07li  K-type  (open chain in slot=%d, %d chars, untouched)  \"%s\"",
								DebugLogGetCycle(), iCurrentFrame, capcode, openSlot,
								g_flexFragSlots[openSlot].textLen, szDbgText);
						else
							DebugLog("[ALPHA] %02d/%03d  capcode=%07li  K-type  \"%s\"",
								DebugLogGetCycle(), iCurrentFrame, capcode, szDbgText);
					}
				}

				hourly_stat[flex_speed][STAT_ALPHA]++;
				daily_stat [flex_speed][STAT_ALPHA]++;

				break;

				case MODE_SHORT_INSTRUCTION:

				// RAH/PH: Short instruction for temporary address in group messaging

				if (!Profile.showinstr) { if (bLongAddress) j++; continue; }

				if (Profile.convert_si) bFLEX_Frame_contains_SI = true;

				strcpy(szWindowText[4], "Groupcall");

				// j<vsa<=88, frame[j+1]<=frame[88] — altijd binnen bounds.
				show_address(frame[j], frame[j+1], bLongAddress);	// show address
				if (bFLEX_groupmessage) { if (bLongAddress) j++; continue; }
				show_phase_speed(vt);

				iAssignedFrame  = (frame[vb] >> 10) & 0x7f;	// Frame with groupmessage
				FlexTempAddress = (frame[vb] >> 17) & 0x7f;	// Listen to this groupcode

				if (!Profile.convert_si)
				{
					display_color(&Pane1, COLOR_INSTRUCTIONS);

					display_show_str(&Pane1, "TEMPORARY ADDRESS");

					w1 = (frame[vb] >> 7) & 0x3;				// See page 90 and 107
	
					if      (w1 == 0) display_show_str(&Pane1, ": ");
					else if (w1 == 7) display_show_str(&Pane1, " (TEST): ");
					else			  display_show_str(&Pane1, " (RESERVED): ");

					sprintf(szTemp, "%07li -> FRAME %03i", FlexTempAddress+2029568, iAssignedFrame);
					display_show_str(&Pane1, szTemp);
				}
				break;

				case MODE_STNUM:
				case MODE_SFNUM:
				case MODE_NUNUM:

				// standard / special format numeric / numbered numeric message

				if (!Profile.shownumeric) { if (bLongAddress) j++; continue; }

				// j<vsa<=88, frame[j+1]<=frame[88] — altijd binnen bounds.
				show_address(frame[j], frame[j+1], bLongAddress);	// show address
				show_phase_speed(vt);

				w1 = frame[vb] >> 7;
				w2 = w1 >> 7;
				w1 = w1 & 0x7f;
				w2 = (w2 & 0x07) + w1;	// numeric message is 7 words max

				if (!bLongAddress)		// load first message word into cc
				{
					cc = frame[w1];		// if short adress first message word @ w1
					w1++;
					w2++;
				}
				else
				{
					// FIX [Berichtdecodering]: boundscheck op frame[vb+1] voor long-address numeric.
					if (vb + 1 >= 200) { j++; continue; }
					cc = frame[vb+1];	// long address - first message word in second vector field
				}

				// skip over first 10 bits for numbered numeric, otherwise skip first 2

				if (vt == 7) m = 14;
				else m = 6;

				for (k=w1; k<=w2; k++)
				{
					if (cc < 0x400000l) display_color(&Pane1, COLOR_NUMERIC);
					else display_color(&Pane1, COLOR_BITERRORS);

					for (l=0; l<21; l++)
					{
						c = c >> 1;

						if ((cc & 0x01) != 0l) c ^= 0x08;

						cc = cc >> 1;
						m--;

						if (m == 0)
						{
							display_show_char(&Pane1, aNumeric[c & 0x0f]);
							hourly_char[flex_speed][STAT_NUMERIC]++;
							daily_char [flex_speed][STAT_NUMERIC]++;
							m = 4;
						}
					}
					cc = (long) frame[k];
				}
				hourly_stat[flex_speed][STAT_NUMERIC]++;
				daily_stat [flex_speed][STAT_NUMERIC]++;

				break;

				case MODE_SH_TONE:

				tt = (frame[vb] >> 7) & 0x03;  // message type

				if ((Profile.showtone && tt) || (Profile.shownumeric && !tt))
				{
					// j<vsa<=88, frame[j+1]<=frame[88] — altijd binnen bounds.
					show_address(frame[j], frame[j+1], bLongAddress);	// show address
					show_phase_speed(vt);

					display_color(&Pane1, COLOR_NUMERIC);

					if (tt)
					{
						display_show_str(&Pane1, "TONE-ONLY");
					}
					else // short numeric (3 or 8 numeric chars)
					{
						for (i=9; i<=17; i+=4)
						{
							cc = (frame[vb] >> i) & 0x0f;
							display_show_char(&Pane1, aNumeric[cc]);
						}

						hourly_char[flex_speed][STAT_NUMERIC] += 3;
						daily_char [flex_speed][STAT_NUMERIC] += 3;

						// FIX [Berichtdecodering]: boundscheck vóór frame[vb+1] voor tone-numeric.
						if (bLongAddress && (vb + 1 < 200))
						{
							for (i=0; i<=16; i+=4)
							{
								cc = (frame[vb+1] >> i) & 0x0f;
								display_show_char(&Pane1, aNumeric[cc]);
							}
							hourly_char[flex_speed][STAT_NUMERIC] += 4;
							daily_char [flex_speed][STAT_NUMERIC] += 4;
						}
					}
					hourly_stat[flex_speed][STAT_NUMERIC]++;
					daily_stat [flex_speed][STAT_NUMERIC]++;
				}
				else { if (bLongAddress) j++; continue; }

				break;

				case MODE_BINARY:

				if (!Profile.showmisc) { if (bLongAddress) j++; continue; }

				// j<vsa<=88, frame[j+1]<=frame[88] — altijd binnen bounds.
				show_address(frame[j], frame[j+1], bLongAddress);	// show address
				show_phase_speed(vt);

				w1 = frame[vb] >> 7;
				w2 = w1 >> 7;
				w1 = w1 & 0x7f;
				w2 = (w2 & 0x7f) + w1 - 1;
				// Same BCH-miscorrection guard as the ALPHA branch above.
				// frame[] is only 200 entries; without the clamp, w2 can reach 253.
				if (w2 > 199) w2 = 199;
				if (w1 > w2)  { if (bLongAddress) j++; continue; }

				if (!bLongAddress)
				{
					iFragmentNumber = (int) (frame[w1] >> 13) & 0x03;

					if (iFragmentNumber == 3) w1+=2;
					else w1++;
				}
				else
				{
					// FIX [Berichtdecodering]: boundscheck op frame[vb+1] voor binary long-address.
					if (vb + 1 >= 200) { j++; continue; }
					iFragmentNumber = (int) (frame[vb+1] >> 13) & 0x03;

					if (iFragmentNumber == 3) w1++;
					w2--;
				}

				display_color(&Pane1, COLOR_MISC);

				for (k=w1, n=0, m=0; k<=w2; k++)
				{
					cc3 = frame[k];

					for (l=0; l<21; l++)
					{
						m = m >> 1;

						if ((cc3 & 0x01l) != 0) m = m ^ 0x08;

						cc3 = cc3 >> 1;
						n++;

						if (n == 4)
						{
							if (m < 10) display_show_char(&Pane1, 48+m);
							else display_show_char(&Pane1, 55+m);

							n=0, m=0;
						}
					}
				}
				break;

				default:
				// vt is `(frame[vb] >> 4) & 0x07` so all 8 values are covered above.
				// This default exists so future code changes don't silently fall through
				// the post-switch ShowMessage()/ConvertGroupcall() path on unknown types.
				if (bLongAddress) j++;
				continue;
			}

			if (Profile.convert_si && ((vt == MODE_SHORT_INSTRUCTION) || bFLEX_groupmessage))
			{
				if (bFLEX_groupmessage)
				{
					if (!bFragmentBuffered)
					{
						DebugLog("[ConvertGroupcall->] %02d/%03d  capcode=%07li  groupbit=%d  vt=%s",
							DebugLogGetCycle(), iCurrentFrame, capcode,
							capcode-2029568, vtype[vt]);
						ConvertGroupcall(capcode-2029568, vtype[vt], capcode);
					}
					else
					{
						// F-type group fragment buffered — skip ConvertGroupcall to prevent premature
						// GroupFrame reset and spurious Y++ on the next fragment.
						DebugLog("[GroupFragBuffered] %02d/%03d  capcode=%07li  groupbit=%d  ConvertGroupcall SKIPPED",
							DebugLogGetCycle(), iCurrentFrame, capcode, capcode-2029568);
						iMessageIndex = 0;
					}
				}
				else
				{
					DebugLog("[AddAssignment->] %02d/%03d  capcode=%07li  groupbit=%d  iAssignedFrame=%d",
						DebugLogGetCycle(), iCurrentFrame, capcode, FlexTempAddress, iAssignedFrame);
					AddAssignment(iAssignedFrame, FlexTempAddress, capcode);
				}
			}
			else if (!bFragmentBuffered)
			{
				ShowMessage();
			}
			else
			{
				iMessageIndex = 0;	// fragment was buffered; discard display buffer without showing
			}

			if (bLongAddress) j++;	// if long address then make sure we skip over both parts
		}
		// Skip when iCurrentFrame is the 99/999 sentinel from a rejected
		// frame-info word — comparing groupcall framenumbers against 999
		// would otherwise count phantom misses.
		if (bCurrentFrameValid) Check4_MissedGroupcalls();
	}
} // Reset for new message.


// format a received frame
void FLEX::showblock(int blknum)
{
	int j, k, err, asa, vsa;
	long int cc;
	static int last_frame;
	bool bNoMoreData=false;	// Speed up frame processing

	for (int i=0; i<8; i++)	// format 32 bit frame into output buffer to do error correction
	{
		for (j=0; j<32; j++)
		{
			k = (j*8) + i;
			ob[j] = block[k];
		}

		err = ecd();		// do error correction
		CountBiterrors(err);

		k = (blknum << 3) + i;

		cc = 0x0000l;

		for (j=0; j<21; j++)
		{
			cc = cc >> 1;
			if (ob[j] == 0) cc ^= 0x100000l;
		}

		if (err >= 3) cc ^= 0x400000l; // flag uncorrectable errors (defensive: ecd() currently returns 0..3)

		frame[k] = cc;
	}
	if ((flex_speed == STAT_FLEX1600) && ((cc == 0x0000l) || (cc == 0x1fffffl)))
	{
		bNoMoreData=true;	// Speed up frame processing
	}

	vsa = (int) ((frame[0] >> 10) & 0x3f);		// get word where vector  field starts (6 bits)
	asa = (int) ((frame[0] >> 8)  & 0x03) + 1;	// get word where address field starts (2 bits)

	if (blknum == 0)
	{
		if (vsa == asa)					// PH: Assuming no messages in current frame,
		{
			bEmpty_Frame=true;
		}
		else
		{
			bEmpty_Frame=false;
		}

		if (!bFlexTIME_detected && !bFlexTIME_not_used)
		{
			FlexTIME();
		}
		else if ((iCurrentFrame == 0) && (last_frame == 127))
		{
			if (Profile.FlexTIME)
			{
				FlexTIME();
			}
		}
		last_frame = iCurrentFrame;
	}
	// show messages in frame if last block was processed and we're not in reflex mode
	else if (((blknum == 10) || bNoMoreData) && !bReflex)
	{
		showframe(asa, vsa);
		if (bNoMoreData) flex_blk=1;
	}
}


// displays given three character word... used when displaying a
// REFLEX message where the characters are spread over multiple phases.
void FLEX::showword(int wordnum)
{
	int c;
	long int cc = (long) frame[wordnum];

	if (cc > 0x200000l) display_color(&Pane1, COLOR_BITERRORS);
	else display_color(&Pane1, COLOR_MESSAGE);

	if ((cc != 0x0000l) && (cc != 0x1fffffl))
	{
		c = (int) cc & 0x7fl;
		display_show_char(&Pane1, c);
		cc = (long) frame[wordnum] >> 7;
		c = (int) cc & 0x7fl;
		display_show_char(&Pane1, c);
		cc = (long) frame[wordnum] >> 14;
		c = (int) cc & 0x7fl;
		display_show_char(&Pane1, c);
	}
}


void FLEX::showwordhex(int wordnum)
{
	display_show_char(&Pane1, '[');
	display_show_hex21(&Pane1, frame[wordnum]);
	display_show_char(&Pane1, ']');
}


void frame_flex(char gin)
{
	int cer, ihd, i, nh, j, hd=0, reflex_flag=0; // te
	static int cy, fr;
	static short int slr[4] = { 0, 0, 0, 0 };
	static int bct, hbit;
	double aver=0.0;

	// flex_reset() asked us to clear our internal state. Do it before any
	// shift-register or counter update so a single stale bit can't slip
	// through into the first post-reset frame.
	if (g_flexFrameFlexResetRequested) {
		cy = 0;
		fr = 0;
		bct = 0;
		hbit = 0;
		slr[0] = slr[1] = slr[2] = slr[3] = 0;
		g_flexFrameFlexResetRequested = false;
	}

	extern double rcver[65];
	extern double exc;
	extern double ct1600, ct3200, ct_bit;
	extern double rcv_clkt;
	extern double rcv_clkt_hi;
	extern double rcv_clkt_fl;

//	unsigned int sup[4] = { 0x870C, 0xA6C6, 0xAAAA, 0x78F3 };	// Main (1600) sync-code

	char sync[64];
	
	extern FLEX phase_A, phase_B, phase_C, phase_D;

	// update bit buffer
	// sync up signal is 1600 BPS 2 level FSK signal
	for (i=0; i<3; i++)
	{
		slr[i] = slr[i] << 1;
		if (slr[i+1] & 0x8000) slr[i] |= 0x0001;
	}
	slr[3] = slr[3] << 1;

	if (gin < 2) slr[3] |= 0x0001;

//	FILE* pTest=NULL;
//
//	if ((pTest = fopen("test.txt", "a")) != NULL)
//	{
//		fwrite(gin < 2 ? "1" : "0", 1, 1, pTest);
//		fclose(pTest);
//	}

	if (flex_blk == 0) // Need sync-up, or just end of transmission?
	{
		if (flex_timer)
		{
			if (nOnes(slr[2] ^ EOT1) + nOnes(slr[3] ^ EOT2) == 0)	// End of transmission?
			{
				flex_timer=0;
				display_showmo(MODE_IDLE);
				return;
			}
		}
		// center portion always the same
		nh = nOnes(slr[1] ^ SYNC1) + nOnes(slr[2] ^ SYNC2);

		if (nh == 32)	// 32 errors, so must be inverted
		{
			if (((slr[0] ^ slr[3]) & 0xFFFF) == 0xFFFF)
			{
				slr[0] ^= 0xFFFF;	// Invert buffer so current frame
				slr[3] ^= 0xFFFF;	// can be decoded
				InvertData();		// Invert receive polarity
				
				nh = 0;		// 32 inverted errors => 0 errors
			}
		}

		// NOTE: STILL MISSING SEVERAL REFLEX SYNC UPS
		// sync up with 1 or less mismatched bits out of center 32
		// AND 2 or less mismatched bits out of outside 32
		if (nh < 2)   // guessing we've gotten sync up...
		{
//			nh = nOnes(slr[0] ^ slr[3]);
			flex_bc = 89;

			if (nOnes(slr[0] ^ slr[3] ^ 0xFFFF) < 2)
			{
				int speed;
				for (speed=0; speed<8; speed++)
				{
					if ((nOnes(slr[0] ^ syncs[speed]) + nOnes(slr[3] ^ ~syncs[speed])) < 2)
					{
						if ((speed & 0x03) == 0)
						{
							if (!Profile.flex_1600) return;		// FLEX-1600
							else flex_speed = STAT_FLEX1600;
						}
						else if ((speed & 0x03) == 0x03)
						{
							if (!Profile.flex_6400) return;		// FLEX-6400
							else flex_speed = STAT_FLEX6400;
						}
						else
						{
							if (!Profile.flex_3200) return;		// FLEX-3200
							else flex_speed = STAT_FLEX3200;
						}

						bFlexActive = true;
						flex_timer  = 20;

						g_sps   = (speed & 0x01) ? 3200 : 1600;
						level   = (speed & 0x02) ? 4 : 2;
						bReflex = (speed & 0x04) ? true : false;

						if (g_sps == 3200) g_sps2 = 3200;

						break;
					}
				}
				if (speed == 8)		// At this point, we should have a valid FLEX-sync, but no match
				{					// in loop above. So let's show as unknown sync header :
					display_color(&Pane1, COLOR_MISC);
					display_line(&Pane1);
					sprintf(sync, " UNKNOWN SYNC HEADER : %hX %hX %hX %hX", slr[0], slr[1], slr[2], slr[3]);
					display_show_strV2(&Pane1, sync);
					display_line(&Pane1);

					return;
				}
			}
			else return;
/*
			if ((nOnes(slr[0] ^ 0x870C) + nOnes(slr[3] ^ 0x78F3)) < 3)	// FLEX 2-level 1600
			{
				if (Profile.flex_1600)	// 1000011100001100-0111100011110011
				{
					bFlexActive = true;
					flex_timer = 21;
					g_sps = 1600;
					flex_speed = STAT_FLEX1600;
					level = 2;
					bReflex = false;
				}
				else return;
			}
			else if ((nOnes(slr[0] ^ 0xB068) + nOnes(slr[3] ^ 0x4F97)) < 3)	// FLEX 4-level 3200
			{
				if (Profile.flex_3200)	// 1011000001101000-0100111110010111
				{
					bFlexActive = true;
					flex_timer = 20;  // 2 seconds
					g_sps = 1600;
					flex_speed = STAT_FLEX3200;
					level = 4;
					bReflex = false;
				}
				else return;
			}
			else if ((nOnes(slr[0] ^ 0xDEA0) + nOnes(slr[3] ^ 0x215F)) < 3)	// FLEX 4-level 6400
			{
				if (Profile.flex_6400)	// 1101111010100000-0010000101011111
				{
					bFlexActive = true;
					flex_timer = 20;  // 2 seconds
					g_sps = 3200;
					g_sps2 = 3200;
					flex_speed = STAT_FLEX6400;
					level = 4;
					bReflex = false;
				}
				else return;
			}
			else if ((nOnes(slr[0] ^ 0x7B18) + nOnes(slr[3] ^ 0x84E7)) < 3)	// FLEX 2-level 3200
			{
				if (Profile.flex_3200)	// 0111101100011000-1000010011100111
				{
					bFlexActive = true;
					flex_timer = 20;	// 2 seconds
					g_sps = 3200;
					g_sps2 = 3200;
					flex_speed = STAT_FLEX3200;
					level = 2;
					bReflex = false;
				}
				else return;
			}
			else if ((nOnes(slr[0] ^ 0x4C7C) + nOnes(slr[3] ^ 0xB383)) < 3)	// REFLEX 4-level 6400
			{
				if (Profile.flex_3200)	// 0100110001111100-1011001110000011
				{
					bFlexActive = true;
					flex_timer = 20;  // 2 seconds
					g_sps = 3200;
					g_sps2 = 3200;
					flex_speed = STAT_FLEX6400;
					level = 4;
					bReflex = true;
				}
				else return;
			}
			else if (((slr[0] ^ slr[3]) & 0xFFFF) == 0xFFFF)
			{
				bFlexActive = true;
				flex_timer = 20;  // 2 seconds
				g_sps = 3200;
				g_sps2 = 3200;
				flex_speed = STAT_FLEX3200;
				level = 2;
				bReflex = true;

				display_color(&Pane1, COLOR_MISC);
				display_line(&Pane1);
				sprintf(sync, " UNKNOWN SYNC HEADER : %X %X %X %X", slr[0], slr[1], slr[2], slr[3]);
				display_show_strV2(&Pane1, sync);
				display_line(&Pane1);
				display_line(&Pane1);
			}
			else return;
*/
			// FINE DIDDLE RCV CLOCK RIGHT HERE****************************
			// idea - take average error over last 64 bits; subtract it off
			// this allows us to go to slower main rcv loop clock response

			// get average rcv clock error over last 64 bits
			for (j=0; j<64; j++) aver = aver + rcver[j];

			aver *= 0.015625;

			// divide by two - just for the heck of it

			aver *= 0.5;
			exc  += aver;

			// go to slower main rcv loop clock

			rcv_clkt = rcv_clkt_fl;
		}

		if (flex_bc > 0)
		{
			flex_bc--;

			// pick off cycle info word (bit numbers 71 to 40 in sync holdoff)
			if ((flex_bc < 72) && (flex_bc > 39))
			{
				if (gin < 2) ob[71-flex_bc] = 1;
				else ob[71-flex_bc] = 0;
			}
			else if (flex_bc == 39)
			{
				if (g_sps2 == 3200)	// Tell soundcard routines we are changing
				{
					cross_over = 1;		// from 1600 to 3200 baud rate, and
					BaudRate = 3200;	// also need to skip some sync-up-data.
				}

				// process cycle info word when its been collected

				cer = ecd();		// do error correction

				if (cer < 2)
				{
					for (ihd = 4; ihd<8; ihd++)
					{
						hd = hd >> 1;
						if (ob[ihd] == 1) hd^=0x08;
					}
					cy = (hd & 0x0f) ^ 0x0f;

					for (ihd = 8; ihd<=14; ihd++)
					{
						hd = hd >> 1;
						if (ob[ihd] == 1) hd^=0x40;
					}
					fr = (hd & 0x7f) ^ 0x7f;
				}
				display_cfstatus(cy, fr);

				bFLEX_Frame_contains_SI = false;

				if (bFlexActive)
				{
					if (bReflex) reflex_flag = 0x10;
					
					if (level == 2)
					{
						if (g_sps == 1600)
						{
							display_showmo(MODE_FLEX_A + reflex_flag);
						}
						else if (g_sps == 3200)
						{
							display_showmo(MODE_FLEX_A + MODE_FLEX_C + reflex_flag);
						}
					}
					else if (level == 4)
					{
						if (g_sps == 1600)
						{
							display_showmo(MODE_FLEX_A + MODE_FLEX_B + reflex_flag);
						}
						else if (g_sps == 3200)
						{
							display_showmo(MODE_FLEX_A + MODE_FLEX_B + MODE_FLEX_C + MODE_FLEX_D + reflex_flag);
						}
					}
				}
			}

			if (flex_bc == 0)
			{
				flex_blk = 11;
				bct = 0;
				hbit = 0;

				// at this point data rate could become either 1600 or 3200 SPS
				if (g_sps == 1600)
				{
					ct_bit = ct1600; // ct_bit = serial port baudrate
					BaudRate = 1600; // BaudRate = soundcard baudrate
				}
				else	// Note we don't change baud rate for soundcard here,
				{		// as we have already changed baud rate. 
					ct_bit = ct3200;
				}
				rcv_clkt = rcv_clkt_hi;	// loosen up rcv loop clock constant again
			}
		}
	}
	else	// update phases depending on transmission speed
	{
		if (g_sps == 1600)
		{
			// always have PHASE A
			if (gin < 2) phase_A.block[bct] = 1;
			else phase_A.block[bct] = 0;

			// if 4 level FSK - do PHASE B also
			if (level == 4)
			{
				if ((gin == 0) || (gin == 3)) phase_B.block[bct] = 1;
				else phase_B.block[bct] = 0;
			}
			bct++;
		}
		else	// split out bits
		{
			if (hbit == 0)
			{
				if (gin < 2) phase_A.block[bct] = 1;
				else phase_A.block[bct] = 0;

				if (level == 4)
				{
					if ((gin == 0) || (gin == 3)) phase_B.block[bct] = 1;
					else phase_B.block[bct] = 0;
				}
				hbit++;
			}
			else
			{
				if (gin < 2) phase_C.block[bct] = 1;
				else phase_C.block[bct] = 0;

				if (level == 4)
				{
					if ((gin == 0) || (gin == 3)) phase_D.block[bct] = 1;
					else phase_D.block[bct] = 0;
				}

				hbit = 0;
				bct++;
			}
		}

		if (bct == 256)
		{
			bct = 0;	   // also pass on block # (0 - 10)

			phase='A';
			phase_A.showblock(11-flex_blk);

			if (level == 4)
			{
				phase='B';
				phase_B.showblock(11-flex_blk);
			}

			if (g_sps == 3200)
			{
				phase='C';
				phase_C.showblock(11-flex_blk);

				if (level == 4)
				{
					phase='D';
					phase_D.showblock(11-flex_blk);
				}
			}

			flex_blk--;

			if (flex_blk == 0)	// if finished set speed back to 1600 sps
			{
				ct_bit = ct1600;  // ct_bit = serial port baudrate
				BaudRate = 1600;  // BaudRate = soundcard baudrate

				bFlexActive = false;

				// if in reflex mode: display raw message if BIW != 0x1fffff
				if (bReflex && (phase_A.frame[0] != 0x1fffffl))
				{
					phase_A.showwordhex(0);

					for (i=0; i<88; i++)
					{
						phase_A.showword(i);
						phase_B.showword(i);
						phase_C.showword(i);
						phase_D.showword(i);
					}
					ShowMessage();
				}
			}
		}
	}
}


// Definition for the forward-declared extern at the top of this file.
// Tracks whether the current cycle/frame indices are real protocol values or
// the 99/999 sentinel set when BCH decoding rejected the frame-info word.
bool bCurrentFrameValid = true;

void display_cfstatus(int cycle, int frame)
{
	// cycle/frame come from the BCH-corrected frame-info word; trust them.
	// A previous hack here advanced the counter when the same frame number arrived
	// twice and contained an SI — but `oldframe` ignored the cycle, the wrap test
	// (frame++ == 128) was post-increment so it never fired, and the mutation made
	// sync drift dependent on whether the prior frame held an SI. Removed.
	if (cycle == 15)
	{
		iCurrentCycle = 99;				// cycle 15 does not exist on-air; display 99/999
		iCurrentFrame = 999;
		bCurrentFrameValid = false;

		CountBiterrors(5);
	}
	else
	{
		iCurrentCycle = cycle;
		iCurrentFrame = frame;
		bCurrentFrameValid = true;
	}
}
// end of display_cfstatus()
