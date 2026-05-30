/*
** rxq.h -- p2kflexDecoder-compatible RX Quality tracker
**
** This is an exact port of the RX quality algorithm from p2kflexDecoder's
** Decode.cpp (struct RxqBucket / RxQualityStats / updateRxQualityFromEcd /
** UpdateRxQualityTimeBased / RXQ_ApplyPenaltyBits / quality()).
**
** Why a separate track from PDW's existing dRX_Quality:
**   PDW has its own RXQ in Misc.cpp CountBiterrors() that drives sigind.cpp
**   and the RX-Q indicator. That algorithm differs significantly from
**   p2kflexDecoder's bucket+EMA scheme — same signal yields different
**   numbers. The Telnet wire-format <TX_STOP><RXQ:NN> must match
**   p2kflexDecoder bit-for-bit for downstream selection logic
**   (p2kflexMonitor master/slave pick) to work, so this module mirrors
**   p2kflexDecoder exactly. PDW's own dRX_Quality is left untouched.
*/
#ifndef PDW_RXQ_H
#define PDW_RXQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Penalty profile for one decoder mode. Values come from
** p2kflexDecoder Decode.cpp lines 167-181. */
typedef struct {
    int dataBits;
    int correctedPenalty;        /* kept for parity; algorithm uses fixed *2 */
    int uncorrectablePenalty;
    int addressPenalty;
} RxqProfile;

extern const RxqProfile RXQ_FLEX;
extern const RxqProfile RXQ_POCSAG;

/* Per-word ecd() outcome integration.
**   err = 0  -> clean
**   err = 1-2 -> corrected
**   err = 3+ -> uncorrectable
** isAddressWord adds addressPenalty for POCSAG address words. */
void Rxq_OnEcd(int err, const RxqProfile *profile, int isAddressWord);

/* Coarse penalty for non-ecd events (CRC failure, BIW xsumchk, etc).
** Maps to p2kflexDecoder's RXQ_ApplyPenaltyBits(). */
void Rxq_ApplyPenaltyBits(uint64_t bits);

/* Recompute EMA + trend. Called at the end of a FLEX frame or a POCSAG
** burst — matches p2kflexDecoder's UpdateRxQualityTimeBased call sites. */
void Rxq_UpdateTimeBased(void);

/* Reset all buckets and EMA to initial state. */
void Rxq_Reset(void);

/* Read current EMA as displayed in <TX_STOP><RXQ:NN>. Range 0..100. */
double Rxq_GetEMA(void);

/* Read current trend character: '+' rising, '-' falling, 0 stable. */
char   Rxq_GetTrend(void);

#ifdef __cplusplus
}
#endif
#endif /* PDW_RXQ_H */
