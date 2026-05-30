/*
** rxq.cpp -- p2kflexDecoder-compatible RX Quality tracker
**
** Exact port of p2kflexDecoder Decode.cpp:165-349. See rxq.h for the
** rationale (Telnet wire-format must match for downstream master/slave
** selection logic in p2kflexMonitor).
**
** Threading: all PDW decoder hooks run on the main thread (WaveIn
** callback path or RxThread → WM_TIMER → pdw_decode). The telnet worker
** thread only READS the EMA in the TX_STOP emit path; on x86/x64 a
** double load is atomic enough for monitoring purposes — a half-updated
** integer value is impossible because doubles align naturally. No lock.
*/

#ifndef STRICT
#define STRICT 1
#endif

#include <windows.h>
#include <string.h>

#include "rxq.h"

/* ---------------------------------------------------------------------------
** Penalty profiles (verbatim from p2kflexDecoder Decode.cpp:167-181)
** ---------------------------------------------------------------------------*/
const RxqProfile RXQ_FLEX   = { 21, 4, 32, 64 };
const RxqProfile RXQ_POCSAG = { 32, 4, 32, 64 };

/* ---------------------------------------------------------------------------
** State — verbatim layout from p2kflexDecoder Decode.h:102-122
** ---------------------------------------------------------------------------*/
typedef struct {
    uint64_t totalBits;
    uint64_t penaltyBits;
    uint64_t uncorrectableBits;
} RxqBucket;

static RxqBucket g_bucket[4];
static uint64_t  g_correctedBits = 0;    /* statistics only */
static uint64_t  g_addressErrors = 0;    /* statistics only */

static volatile double g_rxQualityEMA = 99.5;    /* p2kflex initial value */
static char            g_rxqTrend     = 0;       /* '+', '-' or 0 */

/* ---------------------------------------------------------------------------
** Bucket shift — verbatim from p2kflexDecoder Decode.cpp:188-206
** When bucket[0] fills up, slide all buckets down and halve historical
** penalties so older readings have less weight on quality().
** ---------------------------------------------------------------------------*/
static void RxqCheckShift(void)
{
    const uint64_t MAX_BITS = 10000;

    if (g_bucket[0].totalBits < MAX_BITS) return;

    int i;
    for (i = 3; i > 0; --i) {
        g_bucket[i] = g_bucket[i - 1];
        g_bucket[i].penaltyBits        /= 2;
        g_bucket[i].uncorrectableBits  /= 2;
    }
    memset(&g_bucket[0], 0, sizeof(g_bucket[0]));
}

/* ---------------------------------------------------------------------------
** quality() — verbatim from p2kflexDecoder Decode.cpp:323-341
** ---------------------------------------------------------------------------*/
static double RxqInstantQuality(void)
{
    uint64_t total = 0, bad = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        total += g_bucket[i].totalBits;
        bad   += g_bucket[i].penaltyBits + g_bucket[i].uncorrectableBits;
    }
    if (total == 0)  return 100.0;
    if (bad > total) bad = total;
    return 100.0 * (double)(total - bad) / (double)total;
}

/* ---------------------------------------------------------------------------
** Public API
** ---------------------------------------------------------------------------*/

/* Verbatim port of updateRxQualityFromEcd, p2kflexDecoder Decode.cpp:208-245 */
void Rxq_OnEcd(int err, const RxqProfile *profile, int isAddressWord)
{
    if (!profile) return;

    RxqBucket *b = &g_bucket[0];
    b->totalBits += (uint64_t)profile->dataBits;

    /* Unlock EMA from 100.0 ceiling as soon as ANY error appears */
    if (err > 0 && g_rxQualityEMA > 99.9)
        g_rxQualityEMA = 99.8;

    /* When still in "good signal" mode, double the err so degradation
    ** registers faster. Matches p2kflex behavior. */
    if (g_rxQualityEMA > 95) err *= 2;

    switch (err) {
    case 0:
        break;

    case 1:
    case 2:
        g_correctedBits += err;
        b->penaltyBits  += (uint64_t)(err * 2);
        break;

    default:    /* err == 3 (uncorrectable) or boosted >3 */
        if (isAddressWord) g_addressErrors++;
        b->uncorrectableBits += (uint64_t)profile->dataBits;
        b->penaltyBits       += (uint64_t)profile->uncorrectablePenalty;
        if (isAddressWord)
            b->penaltyBits   += (uint64_t)profile->addressPenalty;
        break;
    }

    RxqCheckShift();
}

/* Verbatim port of RXQ_ApplyPenaltyBits, p2kflexDecoder Decode.cpp:343-349 */
void Rxq_ApplyPenaltyBits(uint64_t bits)
{
    if (g_rxQualityEMA > 95) bits *= 2;

    g_bucket[0].penaltyBits += bits;
    if (g_bucket[0].penaltyBits > g_bucket[0].totalBits)
        g_bucket[0].penaltyBits = g_bucket[0].totalBits;
}

/* Verbatim port of UpdateRxQualityTimeBased, p2kflexDecoder Decode.cpp:248-278 */
void Rxq_UpdateTimeBased(void)
{
    const double oldEMA   = g_rxQualityEMA;
    const double instantQ = RxqInstantQuality();

    /* Asymmetric EMA: instant drop, slow recovery (ALPHA_UP=0.2). */
    const double ALPHA_UP = 0.2;

    double newEMA;
    if (instantQ < g_rxQualityEMA) {
        newEMA = instantQ;                                       /* drop instantly */
    } else {
        newEMA = (1.0 - ALPHA_UP) * g_rxQualityEMA + ALPHA_UP * instantQ;
    }

    if (newEMA >= 99.9) newEMA = 100.0;
    g_rxQualityEMA = newEMA;

    /* Trend — exact match to p2kflex behavior: ANY delta tips +/-. */
    if      (newEMA > oldEMA) g_rxqTrend = '+';
    else if (newEMA < oldEMA) g_rxqTrend = '-';
    else                      g_rxqTrend = 0;
}

void Rxq_Reset(void)
{
    memset(g_bucket, 0, sizeof(g_bucket));
    g_correctedBits = 0;
    g_addressErrors = 0;
    g_rxQualityEMA  = 99.5;
    g_rxqTrend      = 0;
}

double Rxq_GetEMA(void)   { return g_rxQualityEMA; }
char   Rxq_GetTrend(void) { return g_rxqTrend; }
