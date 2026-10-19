/* SPDX-License-Identifier: MIT */
/* Two-stage ordered upload pipeline: core 0 owns descriptor consumption,
 * replay domains and header parsing; core 1 owns token states, bridge-domain
 * insertion, PPE and host delivery. All DMA reads use the uncached alias.
 * Records carry a full station cookie, checked unconditionally before PPE.
 * Replay writes reach DDR before record publication, including on core faults.
 * The SPSC stop handshake must finish before Linux can reuse any RX buffer.
 */
#define UP_ENTRIES 64U
#define UP_WORDS 8U
#define UP_RING 0x3e902800U
#define UP_CTRL (AN7552_NPU_BASE + 0x480U)
#define UP_MAGIC 0x75520a51U
/* Frames core 0 takes per loop iteration: download keeps priority. */
#define UP_FRONT_BUDGET 16U
/* Core 0 publishes the head every UP_PUBLISH records and at the end. */
#define UP_PUBLISH 4U

/* Control words 0..7 are written by core 0 only, 8..15 by core 1 only. */
#define US_HEAD 0	/* core 0: records published (free running) */
#define US_ACTIVE 1	/* core 0: 1 while it owns the RRO ring index */
#define US_FULL 2	/* core 0: calls stopped by a full queue */
#define US_MAGIC 3	/* core 0: UP_MAGIC once booted */
#define US_EPOCH 4	/* core 0: PN key installs and snapshots */
#define US_PN_PAUSE 5	/* core 0: stop back-end readers for a PN command */
#define US_TAIL 8	/* core 1: records consumed (free running) */
#define US_ENABLE 9	/* core 1: core 0 may run the front half */
#define US_DROPPED 10	/* core 1: records dropped at an RRO stop */
#define US_PEAK 11	/* core 1: highest occupancy seen */
#define US_STALE 12	/* core 1: forward verdicts dropped by a key change */
#define US_STOPS 13	/* core 1: completed RRO stop handshakes */
#define US_BACK_ACTIVE 14 /* core 1: inside a PN-sensitive back-end call */

/* Record words. 1..5 line up with rx_metadata() saved[1..5] so that
 * rx_deliver_words() reads either.
 */
#define UR_CLASS 0	/* fast length | offset << 16 | band << 24 | error */
#define UR_D1 1		/* completion descriptor words 1..3 */
#define UR_D2 2
#define UR_D3 3
#define UR_SECURITY 4	/* wcid << 4 | verdict, 0 if not checked */
#define UR_COOKIE 5	/* replay entry word 1 at validation */
#define UR_MAC_LOW 6	/* source address bytes 0..3 (fast candidates) */
#define UR_MAC_HIGH 7	/* bytes 4..5 | epoch << 16 (fast candidates) */
#define UR_FAST_MASK 0x3fffU
#define UR_ERROR 0x80000000U

#define UP_EMPTY 0
#define UP_DONE 1
#define UP_BLOCKED 2
#define UP_ERROR_RECORD 3

_Static_assert(UP_WORDS == 8 && (UP_ENTRIES & (UP_ENTRIES - 1)) == 0,
	       "record queue layout");
_Static_assert(UP_FRONT_BUDGET <= UP_ENTRIES, "front batch fits the queue");
/* Local SRAM: buffer states, PPE index cache, records. */
_Static_assert(RX_STATE_SRAM + AN7552_RX_BUFFER_COUNT + 8 <= UP_RING,
	       "receive state area");
_Static_assert(UP_RING + UP_ENTRIES * UP_WORDS * 4 <= 0x3e903000U,
	       "records fit local SRAM");
/* 0x480-0x4bf is unused by Linux (zeroed at boot only) and the firmware. */
_Static_assert(UP_CTRL + 16 * 4 <= AN7552_NPU_BASE + 0x4c0U,
	       "control row 480");

/* Host tests route the shared accesses through a weak-memory model. */
#ifndef SPLIT_LD
#define SPLIT_LD(p) (*(p))
#define SPLIT_ST(p, v) (*(p) = (v))
#endif

static inline volatile uint32_t *up_ctrl(void)
{
	return (volatile uint32_t *)(uintptr_t)UP_CTRL;
}

static inline volatile uint32_t *up_slot(uint32_t index)
{
	return (volatile uint32_t *)(uintptr_t)UP_RING +
	       (index % UP_ENTRIES) * UP_WORDS;
}

/* Boot, before AN7552_MAGIC: each core clears its own words. Linux zeroed
 * 0x000-0x7ff before releasing the cores and enables the RRO only after
 * both announced AN7552_MAGIC.
 */
__attribute__((unused)) static void up_init(uint32_t core)
{
	volatile uint32_t *s = up_ctrl();

	if (!core) {
		s[US_HEAD] = 0;
		s[US_ACTIVE] = 0;
		s[US_FULL] = 0;
		s[US_EPOCH] = 0;
		s[US_PN_PAUSE] = 0;
		s[US_MAGIC] = UP_MAGIC;
		return;
	}
	s[US_TAIL] = 0;
	s[US_ENABLE] = 0;
	s[US_DROPPED] = 0;
	s[US_PEAK] = 0;
	s[US_STALE] = 0;
	s[US_STOPS] = 0;
	s[US_BACK_ACTIVE] = 0;
	RX_PPE_CACHE[1] = RX_PPE_STALE;
}

/* ---------------------------------------------------------------- core 0 */

struct up_front {
	uint32_t head;		/* own copy of US_HEAD */
	uint32_t halted;	/* an error record is out; wait for a stop */
};

struct up_front_batch {
	struct pn_cache pn;
	uint32_t index, phase, chain, mode, epoch, secure, done;
};

/* Core 0 owns replay state, but core 1 can still be using an old cookie.
 * Fence the pause before checking its ACTIVE word (Dekker with back_enter).
 * Do not acknowledge the host command until every earlier back-end call
 * finished. This also prevents snapshots racing with a late PPE submission.
 */
__attribute__((unused)) static void up_pn_service(void)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + PN_CONTROL);
	volatile uint32_t *s = up_ctrl();

	if (!q[0] || q[0] == q[8] || q[9]) {
		SPLIT_ST(&s[US_PN_PAUSE], 0);
		return;
	}
	SPLIT_ST(&s[US_PN_PAUSE], 1);
	barrier();
	if (SPLIT_LD(&s[US_BACK_ACTIVE]))
		return;
	barrier();
	native_pn_service();
	barrier();
	SPLIT_ST(&s[US_PN_PAUSE], 0);
}

/* A station pause happens between complete front batches. Clearing ACTIVE
 * here lets an overlapping RRO stop finish while the station pause is held.
 */
__attribute__((unused)) static void up_front_pause(void)
{
	barrier();
	SPLIT_ST(&up_ctrl()[US_ACTIVE], 0);
	barrier();
}

static int up_front_one(struct up_front_batch *b, volatile uint32_t *rec)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *descriptor;
	struct rx_metadata parsed;
	struct pkt_view view;
	uint32_t d0, d1, d2, d3, length, token, reason, security, parsed_result,
		cookie = 0, class = 0, mac_low = 0, mac_high = 0;

	parsed.wcid = parsed.band = parsed.bss = 0;
	parsed.offset = parsed.length = 0;
	descriptor = rx_words(AN7552_RX_COMPLETE_HOST + b->index * 16);
	d3 = descriptor[3];
	if ((d3 >> 28) != b->phase)
		return UP_EMPTY;
	barrier();
	d0 = descriptor[0];
	d1 = descriptor[1];
	d2 = descriptor[2];
	length = (d1 >> 16) & 16383;
	token = (d2 >> 16) - AN7552_RX_TOKEN_BASE;
	reason = (d2 >> 12) & 15;
	/* The buffer state check is core 1's (it owns the states). */
	if (token >= AN7552_RX_BUFFER_COUNT || length > 2048 ||
	    d0 != AN7552_RX_BUFFER_HOST + token * 2048) {
		SPLIT_ST(&rec[UR_D1], d1);
		SPLIT_ST(&rec[UR_D2], d2);
		SPLIT_ST(&rec[UR_CLASS], UR_ERROR);
		return UP_ERROR_RECORD;
	}
	/* pkt_view reads each header word once through the uncached DMA alias. */
	pv_init(&view, (const volatile uint8_t *)rx_words(d0), length);
	/* A buffer after a non-final one continues the previous MPDU. */
	if (b->chain || !(d1 & (1U << 30)))
		reason = 1;
	security = (reason == 1 || reason == 2) ?
			   0 :
			   native_rx_security_view(&view, &cookie, &b->pn);
	b->secure += ((security & 15) == 1);
	if ((security & 15) == 2)
		q[24]++;
	parsed_result = rx_parse_view(&view, reason, &parsed);
	if ((security & 15) == 1 && parsed_result != RX_PARSE_OK)
		(*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE + 0x780 +
						   parsed_result * 4))++;
	/* Forwarding candidate: validated, single buffer, parsed, matching
	 * station. Core 1 checks the identity against the station table.
	 */
	if (b->mode == AN7552_PPE_MODE_BRIDGE && (security & 15) == 1 && (d1 & (1U << 30)) &&
	    parsed_result == RX_PARSE_OK && parsed.wcid == (security >> 4) &&
	    parsed.band < 2) {
		uint32_t source = parsed.offset + 6;

		class = parsed.length | (parsed.offset << 16) |
			(parsed.band << 24);
		mac_low = pv_get32(&view, source);
		mac_high = pv_byte(&view, source + 4) |
			   (pv_byte(&view, source + 5) << 8) |
			   (b->epoch << 16);
	}
	SPLIT_ST(&rec[UR_D1], d1);
	SPLIT_ST(&rec[UR_D2], d2);
	SPLIT_ST(&rec[UR_D3], d3);
	SPLIT_ST(&rec[UR_SECURITY], security);
	SPLIT_ST(&rec[UR_COOKIE], cookie);
	SPLIT_ST(&rec[UR_MAC_LOW], mac_low);
	SPLIT_ST(&rec[UR_MAC_HIGH], mac_high);
	SPLIT_ST(&rec[UR_CLASS], class);
	/* Data1 bit 30 marks the last buffer of an MPDU. */
	b->chain = !(d1 & (1U << 30));
	b->index++;
	if (b->index == AN7552_RX_RING_COUNT) {
		b->index = 0;
		b->phase = (b->phase + 1) & 15;
	}
	b->done++;
	return UP_DONE;
}

/* Core 0, once per loop iteration, at most UP_FRONT_BUDGET frames. Never
 * keeps a record unpublished across calls, so parking cannot lose one.
 */
__attribute__((unused)) static void native_up_front(struct up_front *f)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *ppe = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	volatile uint32_t *s = up_ctrl();
	struct up_front_batch b;
	uint32_t tail, room, head, published, result = UP_EMPTY;

	if (!SPLIT_LD(&s[US_ENABLE])) {
		if (SPLIT_LD(&s[US_ACTIVE])) {
			/* Every published head before core 1 may see us idle. */
			barrier();
			SPLIT_ST(&s[US_ACTIVE], 0);
		}
		return;
	}
	if (!SPLIT_LD(&s[US_ACTIVE])) {
		/* Dekker with up_quiet(): store ACTIVE, fence, load ENABLE. */
		SPLIT_ST(&s[US_ACTIVE], 1);
		barrier();
		if (!SPLIT_LD(&s[US_ENABLE])) {
			SPLIT_ST(&s[US_ACTIVE], 0);
			return;
		}
		f->head = SPLIT_LD(&s[US_HEAD]);
		f->halted = 0;
	}
	if (f->halted || q[6] || !q[4])
		return;
	if (SPLIT_LD(&s[US_PN_PAUSE]))
		return;
	head = published = f->head;
	tail = SPLIT_LD(&s[US_TAIL]);
	/* A stale tail only makes the queue look fuller. */
	if (head - tail > UP_ENTRIES)
		return;
	room = UP_ENTRIES - (head - tail);
	if (!room) {
		SPLIT_ST(&s[US_FULL], SPLIT_LD(&s[US_FULL]) + 1);
		return;
	}
	if (room > UP_FRONT_BUDGET)
		room = UP_FRONT_BUDGET;
	pc_reset(&b.pn);
	b.index = q[16];
	b.phase = q[17];
	b.chain = q[30];
	b.mode = ppe[0];
	b.epoch = s[US_EPOCH];
	b.secure = 0;
	b.done = 0;
	for (uint32_t n = 0; n < room; n++) {
		result = up_front_one(&b, up_slot(head));
		if (result == UP_EMPTY)
			break;
		head++;
		if (result == UP_ERROR_RECORD) {
			f->halted = 1;
			break;
		}
		if (head - published >= UP_PUBLISH) {
			/* The records are visible before the head covering them. */
			barrier();
			SPLIT_ST(&s[US_HEAD], head);
			published = head;
		}
	}
	if (b.secure)
		q[23] += b.secure;
	if (b.done) {
		/* Descriptor reads complete before the radio may reuse slots. */
		barrier();
		*rx_words(AN7552_RX_EMI_HOST) =
			b.index ? b.index - 1 : AN7552_RX_RING_COUNT - 1;
		q[16] = b.index;
		q[17] = b.phase;
		q[30] = b.chain;
	}
	if (head != published) {
		barrier();
		SPLIT_ST(&s[US_HEAD], head);
	}
	f->head = head;
}

/* ---------------------------------------------------------------- core 1 */

struct up_back {
	struct rro_batch r;	/* PPE queue state, identity cache, done */
	uint32_t head, tail, published;
};

/* Core 1: stop core 0 and empty the queue. Returns 1 once core 0 is idle
 * and nothing is queued. Dropped records name buffers in receive state 1;
 * Linux re-initialises all of them before the next start.
 */
static int up_quiet(void)
{
	volatile uint32_t *s = up_ctrl();
	uint32_t head, tail;

	if (SPLIT_LD(&s[US_ENABLE]))
		SPLIT_ST(&s[US_ENABLE], 0);
	/* Store ENABLE before loading ACTIVE (Dekker with native_up_front). */
	barrier();
	if (SPLIT_LD(&s[US_ACTIVE]))
		return 0;
	/* Core 0 fenced its last head before clearing ACTIVE. */
	barrier();
	head = SPLIT_LD(&s[US_HEAD]);
	tail = SPLIT_LD(&s[US_TAIL]);
	if (head != tail) {
		s[US_DROPPED] += head - tail;
		SPLIT_ST(&s[US_TAIL], head);
		barrier();
	}
	return 1;
}

/* Core 1, after a (re)start: drop anything stale, then let core 0 run. */
static int up_start(void)
{
	volatile uint32_t *s = up_ctrl();
	uint32_t head, tail;

	if (SPLIT_LD(&s[US_ACTIVE]))
		return 0;
	barrier();
	head = SPLIT_LD(&s[US_HEAD]);
	tail = SPLIT_LD(&s[US_TAIL]);
	if (head != tail) {
		s[US_DROPPED] += head - tail;
		SPLIT_ST(&s[US_TAIL], head);
	}
	barrier();
	SPLIT_ST(&s[US_ENABLE], 1);
	return 1;
}

static void up_fail(uint32_t d1, uint32_t d2, uint32_t token)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);

	q[8] = (d1 >> 16) & 16383;
	q[9] = (d2 >> 12) & 15;
	q[15] = token;
	q[6] = 3;
}

static int up_back_one(struct up_back *u)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *s = up_ctrl(), *rec, *station, *entry;
	uint32_t class, d1, d2, token, state, band, retry, fast, security = 0,
		 mac_high;

	if (u->tail == u->head) {
		u->head = SPLIT_LD(&s[US_HEAD]);
		if (u->tail == u->head)
			return UP_EMPTY;
		/* Records were published before the head that covers them. */
		barrier();
	}
	rec = up_slot(u->tail);
	class = SPLIT_LD(&rec[UR_CLASS]);
	d1 = SPLIT_LD(&rec[UR_D1]);
	d2 = SPLIT_LD(&rec[UR_D2]);
	token = (d2 >> 16) - AN7552_RX_TOKEN_BASE;
	if (class & UR_ERROR) {
		up_fail(d1, d2, token);
		return UP_EMPTY;
	}
	state = token < AN7552_RX_BUFFER_COUNT ? st_get(token) : 0;
	if ((state & 255) != 1) {
		up_fail(d1, d2, token);
		return UP_EMPTY;
	}
	band = (state >> 8) & 1;
	/* Set when an earlier attempt on this record was blocked. */
	retry = state & 0x10000;
	fast = class & UR_FAST_MASK;
	if (fast) {
		uint32_t wcid, eligible;

		security = SPLIT_LD(&rec[UR_SECURITY]);
		wcid = security >> 4;
		station = (void *)(uintptr_t)(AN7552_STATION_NPU + wcid * 32);
		eligible = station[0];
		/* Words 1..3 change only under the station quiesce, which never
		 * overlaps a core 1 call. Word 0 is read live.
		 */
		if (eligible && u->r.st_wcid != wcid) {
			u->r.st1 = station[1];
			u->r.st2 = station[2];
			u->r.st3 = station[3];
			u->r.st_wcid = wcid;
		}
		mac_high = SPLIT_LD(&rec[UR_MAC_HIGH]);
		if (!(eligible & AN7552_STATION_AUTHORIZED) ||
		    ((u->r.st1 & 255) != ((class >> 24) & 3) &&
		     !(eligible & AN7552_STATION_MLO)) ||
		    u->r.st2 != SPLIT_LD(&rec[UR_MAC_LOW]) ||
		    u->r.st3 != (mac_high & 65535)) {
			if (!retry)
				q[27]++;
			fast = 0;
		}
#ifdef UP_TEST_AFTER_IDENTITY_CHECK
		UP_TEST_AFTER_IDENTITY_CHECK();
#endif
		if (fast) {
			/* A queued record can outlive any number of unrelated key
			 * updates. Always compare its full per-station cookie.
			 */
			entry = pn_entry(wcid);
			if (!entry[0]) {
				fast = 0;
			} else {
				barrier();
				if (entry[1] != SPLIT_LD(&rec[UR_COOKIE])) {
					s[US_STALE]++;
					fast = 0;
				}
			}
		}
#ifdef UP_TEST_AFTER_RECHECK
		UP_TEST_AFTER_RECHECK(fast, wcid, rec);
#endif
	}
	volatile uint32_t *saved = rx_metadata(token);
	uint32_t address = AN7552_RX_BUFFER_HOST + token * 2048;
	if (!retry)
		rx_domain(token)->active = 0;
	saved[0] = address;
	saved[1] = d1;
	saved[2] = d2;
	saved[3] = SPLIT_LD(&rec[UR_D3]);
	saved[4] = SPLIT_LD(&rec[UR_SECURITY]);
	saved[5] = SPLIT_LD(&rec[UR_COOKIE]);
	saved[6] = (class >> 16) & 255;
	saved[7] = fast;
	if (fast) {
		int ret = native_ppe_queue(&u->r, token, address + saved[6], fast);
		if (!ret) {
			st_set(token, state | 0x10000);
			return UP_BLOCKED;
		}
		if (ret > 0)
			st_set(token, 2 | (band << 8));
		else
			fast = saved[7] = 0;
	}
	if (!fast) {
		volatile uint32_t *ppe = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
		native_ppe_flush(&u->r);
		if (ppe[12] || !native_rx_deliver(token, 0)) {
			st_set(token, state | 0x10000);
			return UP_BLOCKED;
		}
	}

	u->tail++;
	u->r.done++;
	return UP_DONE;
}

static void up_back_flush(struct up_back *u)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);

	native_ppe_flush(&u->r);
	if (u->r.done) {
		q[7] += u->r.done;
		u->r.done = 0;
	}
	if (u->tail != u->published) {
		/* Record reads complete before the slots are handed back. */
		barrier();
		SPLIT_ST(&up_ctrl()[US_TAIL], u->tail);
		u->published = u->tail;
	}
}

/* Stop-only work is safe during a station pause: it never reads a station
 * identity, replay entry or packet. In particular, a paused core 0 must not
 * keep Linux waiting for q[5] merely because the front ACTIVE bit was set.
 */
static int up_back_stopped(void)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *s = up_ctrl();

	if (q[4])
		return 0;
	if (!q[5] && !SPLIT_LD(&s[US_ENABLE])) {
		RX_PPE_CACHE[1] = RX_PPE_STALE;
		return 1;
	}
	if (up_quiet()) {
		if (q[5])
			s[US_STOPS]++;
		q[5] = 0;
	}
	RX_PPE_CACHE[1] = RX_PPE_STALE;
	return 1;
}

/* Core 1, once per loop iteration, outside the station quiesce. */
__attribute__((unused)) static void native_up_back(void)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *ppe = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	volatile uint32_t *s = up_ctrl();
	struct up_back u;
	int result;

	if (up_back_stopped())
		return;
	if (!q[5]) {
		if (!up_quiet())
			return;
		st_load();
	}
	q[5] = 1;
	if (q[6])
		return;
	if (!SPLIT_LD(&s[US_ENABLE]) && !up_start())
		return;
	native_rx_refill(0);
	native_rx_refill(1);
	u.r.mode = ppe[0];
	u.r.done = 0;
	u.r.ppe_producer = ppe[10];
	rro_ppe_hw_load(&u.r);
	u.r.ppe_pending = 0;
	u.r.st_wcid = 0xffffffff;
	u.tail = u.published = SPLIT_LD(&s[US_TAIL]);
	u.head = SPLIT_LD(&s[US_HEAD]);
	barrier();
	if (u.head - u.tail > s[US_PEAK])
		s[US_PEAK] = u.head - u.tail;
	for (uint32_t n = 0; n < RRO_BATCH; n++) {
		result = up_back_one(&u);
		if (result == UP_DONE)
			continue;
		if (result == UP_EMPTY || q[6])
			break;
		/* PPE input/completions or host ring blocked: drain once and
		 * retry the same record.
		 */
		up_back_flush(&u);
		native_ppe_service();
		u.r.ppe_hw = read32(0x1fb5080c);
		result = q[6] ? UP_EMPTY : up_back_one(&u);
		if (result != UP_DONE)
			break;
	}
	up_back_flush(&u);
	/* ppe[10] now equals u.r.ppe_producer. */
	rro_ppe_hw_save(&u.r);
}

static int up_back_enter(void)
{
	volatile uint32_t *s = up_ctrl();

	if (SPLIT_LD(&s[US_PN_PAUSE]))
		return 0;
	SPLIT_ST(&s[US_BACK_ACTIVE], 1);
	barrier();
	if (SPLIT_LD(&s[US_PN_PAUSE])) {
		SPLIT_ST(&s[US_BACK_ACTIVE], 0);
		barrier();
		return 0;
	}
	return 1;
}

static void up_back_exit(void)
{
	/* All packet/PN reads and PPE doorbells finish before releasing the
 * back end to a host PN command running on core 0.
 */
	barrier();
	SPLIT_ST(&up_ctrl()[US_BACK_ACTIVE], 0);
	barrier();
}

/* Production entry point: protect both PPE completions and new submissions.
 * No spinning: a pending PN request progresses on the other core's next loop.
 */
__attribute__((unused)) static void native_up_service(void)
{
	if (!up_back_enter())
		return;
	native_ppe_service();
	native_up_back();
	up_back_exit();
}
