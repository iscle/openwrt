/* SPDX-License-Identifier: MIT */
/* Upload (Wi-Fi -> Ethernet) receive path as a two-stage pipeline.
 *
 * Stage 1, core 0 (native_up_front): read the RRO completion descriptor,
 * check the replay counter (security view, PN cache), parse the frame and
 * classify it for the PPE. The result is one 32-byte record per completion
 * descriptor, pushed in ring order to a single-producer single-consumer queue
 * in SRAM. Then the completion slots go back to the radio (EMI index).
 *
 * Stage 2, core 1 (native_up_back): take records in order, check the buffer
 * state, check the station identity and the live key word, then queue the
 * frame to the PPE or copy it to Linux, release buffers, refill the radio
 * receive rings and pop the PPE return queue (ppe.c.inc).
 *
 * Why this cut: everything that reads the packet header (security + parse)
 * stays on one core, so header words are fetched once. Everything that
 * changes buffer ownership (state bytes, free list, refill, PPE ring, Linux
 * ring) stays on core 1. Core 0 never reads or writes the buffer state
 * bytes, so the pipeline does not depend on core 0 seeing core 1's state
 * array; core 1 checks the state of every token it is handed. The station
 * identity check moves to core 1: core 1 reads the eligibility word right
 * before it queues the frame anyway (the old "fast recheck"), and its
 * per-call identity cache never spans a station quiesce because core 1 skips
 * the back half while one is in progress. Core 0 does not use the station
 * table at all.
 *
 * Ownership (one writer per word; "others" lists the other readers):
 *   state                                owner   others
 *   RRO completion ring read index,      core 0  -
 *     phase, chain (q[16], q[17], q[30]),
 *     EMI consumer index (radio)
 *   replay table counters and PN         core 0  core 1 reads entry words 0
 *     service (PN_CONTROL), PN cache                and 1 only; Linux word 0
 *   US_EPOCH (key install/snapshot cnt)  core 0  core 1
 *   q[23] secure, q[24] PN fail,         core 0  Linux
 *     parse counters 0x780..
 *   record slots between tail and head   core 0 writes, core 1 reads
 *   US_HEAD, US_ACTIVE, US_FULL          core 0  core 1
 *   US_TAIL, US_ENABLE, US_DROPPED, ...  core 1  core 0 (TAIL, ENABLE)
 *   RX buffer state bytes (SRAM)         core 1  -
 *   free list, q[20], q[21], q[22]       core 1  Linux at setup only
 *   radio refill rings                   core 1  -
 *   rx_metadata saved[1..5]              core 1  core 1 (PPE return path)
 *   PPE ring, q[1], q[10], q[12], cache  core 1  -
 *   PPE return pop (fe4/fe0/fdc)         core 1  -
 *   Linux ring q[2], q[3], data[2], [7]  core 1  Linux
 *   q[5] (RRO active ack), q[6] (error), core 1  Linux, core 0 reads q[6]
 *     q[7] (frames done), q[8], q[9],
 *     q[15], q[27] (identity mismatch)
 *   station identity cache               core 1  (per call)
 *
 * Ordering: one FIFO, one producer, one consumer, so frames leave in the
 * completion ring order, as with one core (a frame that cannot be handed on
 * blocks the ones behind it). A frame for Linux still waits until the PPE
 * returned every earlier frame (ppe[12] == 0), as before.
 *
 * Back-pressure: core 0 consumes a completion descriptor only for a free
 * record slot. With the queue full it stops reading the RRO ring (US_FULL
 * counts such calls), the ring fills and the radio holds frames, exactly as
 * when one core was too slow. Nothing is dropped.
 *
 * Errors: a descriptor whose token, length or address is invalid becomes an
 * error record (index not advanced); core 0 stops, core 1 reaches the record
 * in order and latches q[6] = 3 with q[8], q[9], q[15] as before. Core 1
 * latches the buffer state check (q[6] = 3) and all its own errors; core 0
 * stops producing once q[6] != 0.
 *
 * RRO stop (Linux q[4] = 0, waits for q[5] == 0): core 1 clears US_ENABLE,
 * fences and waits until core 0 dropped US_ACTIVE (Dekker pair). Only then
 * does it drop the queued records (US_DROPPED) and write q[5] = 0. Linux
 * re-initialises every buffer state, the free list and the rings in
 * rro_setup() before the next q[4] = 1, as it did for a frame parked by the
 * single-core code. Restart: core 1 reloads the states, drops anything stale
 * and sets US_ENABLE; core 0 reads the ring index from q[16] (zeroed by
 * Linux) on its next call.
 *
 * Station quiesce: core 1 runs the back half only outside the handshake and
 * checks identity against the live table. Core 0 does not touch the table.
 *
 * Replay table changes: core 0 runs the PN service between its batches. A
 * key install (op 2) or snapshot (op 3) bumps US_EPOCH before it touches the
 * entry. A record carries the low 16 epoch bits for diagnostics;
 * core 1 always compares the record's cookie with entry word 1, exactly
 * as the single-core code re-checked a parked frame.
 *
 * Shared memory: control words at NPU_BASE + 0x480 (debugfs status row
 * "480:"), records in the SRAM at UP_RING, behind the buffer states and the
 * PPE index cache that core 1 keeps there. Both harts see that SRAM.
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
#define US_TAIL 8	/* core 1: records consumed (free running) */
#define US_ENABLE 9	/* core 1: core 0 may run the front half */
#define US_DROPPED 10	/* core 1: records dropped at an RRO stop */
#define US_PEAK 11	/* core 1: highest occupancy seen */
#define US_STALE 12	/* core 1: forward verdicts dropped by a key change */
#define US_STOPS 13	/* core 1: completed RRO stop handshakes */

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
		s[US_MAGIC] = UP_MAGIC;
		return;
	}
	s[US_TAIL] = 0;
	s[US_ENABLE] = 0;
	s[US_DROPPED] = 0;
	s[US_PEAK] = 0;
	s[US_STALE] = 0;
	s[US_STOPS] = 0;
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

/* Core 0: PN service between front batches (replay table owner). */
__attribute__((unused)) static void up_pn_service(void)
{
	pn_service_run(&up_ctrl()[US_EPOCH]);
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
	pv_init(&view, RX_HEADER_ALIAS(d0), length);
	/* A buffer after a non-final one continues the previous MPDU. */
	if (b->chain)
		reason = 1;
	security = (reason == 1 || reason == 2) ?
			   0 :
			   native_rx_security_view(&view, length, &cookie,
						   &b->pn);
	b->secure += ((security & 15) == 1);
	if ((security & 15) == 2)
		q[24]++;
	parsed_result = rx_parse_view(&view, length, reason, &parsed);
	if ((security & 15) == 1 && parsed_result != RX_PARSE_OK)
		(*(volatile uint32_t *)(uintptr_t)(AN7552_NPU_BASE + 0x780 +
						   parsed_result * 4))++;
	/* Forwarding candidate: validated, single buffer, parsed, matching
	 * station. Core 1 checks the identity against the station table.
	 */
	if (b->mode == 2 && (security & 15) == 1 && (d1 & (1U << 30)) &&
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
	head = published = f->head;
	tail = SPLIT_LD(&s[US_TAIL]);
	/* A stale tail only makes the queue look fuller. */
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
	pc_flush(&b.pn);
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
		if (!eligible || (u->r.st1 & 255) != ((class >> 24) & 3) ||
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
	if (fast) {
		volatile uint32_t *saved = rx_metadata(token);
		uint32_t address = AN7552_RX_BUFFER_HOST + token * 2048;

		/* The PPE return path delivers from saved[1..5]. */
		saved[1] = d1;
		saved[2] = d2;
		saved[3] = SPLIT_LD(&rec[UR_D3]);
		saved[4] = security;
		saved[5] = SPLIT_LD(&rec[UR_COOKIE]);
		if (!native_ppe_queue(&u->r, token,
				      address + ((class >> 16) & 255), fast)) {
			st_set(token, state | 0x10000);
			return UP_BLOCKED;
		}
		st_set(token, 2 | (band << 8));
	} else {
		volatile uint32_t *ppe =
			(void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);

		/* Earlier PPE input may still need delivery to the same host
		 * ring. Publish outstanding work and let the retry drain it.
		 */
		native_ppe_flush(&u->r);
		if (ppe[12] || !rx_deliver_words(token, band, rec, 0)) {
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

/* Core 1, once per loop iteration, outside the station quiesce. */
__attribute__((unused)) static void native_up_back(void)
{
	volatile uint32_t *q =
		(void *)(uintptr_t)(AN7552_NPU_BASE + AN7552_RRO_CONTROL);
	volatile uint32_t *ppe = (void *)(uintptr_t)(AN7552_NPU_BASE + 0x6c0);
	volatile uint32_t *s = up_ctrl();
	struct up_back u;
	int result;

	if (!q[4]) {
		/* Already stopped: core 0 stays idle until US_ENABLE. */
		if (!q[5] && !SPLIT_LD(&s[US_ENABLE])) {
			RX_PPE_CACHE[1] = RX_PPE_STALE;
			return;
		}
		/* q[5] = 0 only once core 0 stopped and the queue is empty. */
		if (up_quiet()) {
			if (q[5])
				s[US_STOPS]++;
			q[5] = 0;
		}
		RX_PPE_CACHE[1] = RX_PPE_STALE;
		return;
	}
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
