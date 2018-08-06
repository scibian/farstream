/*
 * Farstream - Farstream TFRC implementation
 *
 * Copyright 2010 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2010 Nokia Corp.
 *
 * tfrc.c - An implemention of TCP Friendly rate control, RFCs 5348 and 4828
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "tfrc.h"

#include <math.h>
#include <string.h>

/* for gst_util_uint64_scale_round */
#include <gst/gst.h>

/*
 * ALL TIMES ARE IN MICROSECONDS
 * bitrates are in bytes/sec
 */

#if 0
#define DEBUG_RECEIVER(receiver, format, ...) \
  g_debug ("TFRC-R (%p): " format , receiver,  __VA_ARGS__)
#else
#define DEBUG_RECEIVER(...)
#endif

#if 0
#define DEBUG_SENDER(sender, format, ...) \
  g_debug ("TFRC-S (%p): " format,  sender,  __VA_ARGS__)
#else
#define DEBUG_SENDER(...)
#endif

#define DEFAULT_MSS 1460

#define SECOND (1000 * 1000)

#define MIN_NOFEEDBACK_TIMER (20 * 1000)

/*
 * @s: segment size in bytes
 * @R: RTT in milli seconds (instead of seconds)
 * @p: loss event per packet transmitted
 *
 *                              s
 * X_Bps = -----------------------------------------------
 *         R * (sqrt(2*p/3) + 12*sqrt(3*p/8)*p*(1+32*p^2))
 *
 * Returns: The bitrate in bytes/s
 */
static gdouble
calculate_bitrate (gdouble s, gdouble R, gdouble p)
{
  gdouble f = sqrt (2 * p / 3) + 12 * sqrt (3 * p / 8) * p * (1 + 32 * p * p);

  return (SECOND * s) / (R * f);
}

#define RECEIVE_RATE_HISTORY_SIZE      (4)

/* Specified in RFC 4828 Section 3 second bullet */
#define HEADER_SIZE     (40)

struct ReceiveRateItem {
  guint64 timestamp;
  guint rate;
};

struct _TfrcSender {
  guint computed_rate; /* The rate computer from the TCP throughput equation */

  gboolean sp;
  guint average_packet_size; /* 16 times larger */
  gboolean use_inst_rate; /* use inst_rate instead of rate */

  guint mss; /* max segment size */
  guint rate; /* maximum allowed sending rate in bytes/sec */
  guint inst_rate; /* corrected maximum allowed sending rate */
  guint averaged_rtt;
  guint sqmean_rtt;
  guint last_sqrt_rtt;
  guint64 tld; /* Time Last Doubled during slow-start */

  guint64 nofeedback_timer_expiry;

  guint retransmission_timeout; /* RTO */

  struct ReceiveRateItem receive_rate_history[RECEIVE_RATE_HISTORY_SIZE];

  gdouble last_loss_event_rate;

  gboolean sent_packet;
};

TfrcSender *
tfrc_sender_new (guint segment_size, guint64 now, guint initial_rate)
{
  TfrcSender *sender = g_slice_new0 (TfrcSender);

  /* initialized as described in RFC 5348 4.2 */
  sender->use_inst_rate = TRUE;
  sender->mss = DEFAULT_MSS;
  sender->average_packet_size = segment_size << 4;
  if (initial_rate)
    sender->rate = initial_rate;
  else
    sender->rate = segment_size;

  sender->retransmission_timeout = 2 * SECOND;
  sender->nofeedback_timer_expiry = now + sender->retransmission_timeout;
  return sender;
}

TfrcSender *
tfrc_sender_new_sp (guint64 now, guint initial_average_packet_size)
{
  TfrcSender *sender = tfrc_sender_new (1460, now, 0);

  sender->sp = TRUE;

  sender->average_packet_size = initial_average_packet_size << 4;

  return sender;
}

void
tfrc_sender_use_inst_rate (TfrcSender *sender, gboolean use_inst_rate)
{
  sender->use_inst_rate = use_inst_rate;
}


void
tfrc_sender_free (TfrcSender *sender)
{
  g_slice_free (TfrcSender, sender);
}

static guint
sender_get_segment_size (TfrcSender *sender)
{
  if (sender->sp)
    return sender->mss;
  else
    return sender->average_packet_size >> 4;
}

void
tfrc_sender_on_first_rtt (TfrcSender *sender, guint64 now)
{
  sender->receive_rate_history[0].rate = G_MAXUINT;
  sender->receive_rate_history[0].timestamp = now;
}

static guint get_max_receive_rate (TfrcSender *sender, gboolean ignore_max_uint)
{
  guint max_rate = 0;
  guint i;

  for (i = 0; i < RECEIVE_RATE_HISTORY_SIZE; i++)
  {
    if (G_UNLIKELY (sender->receive_rate_history[i].rate == G_MAXUINT))
    {
      if (ignore_max_uint)
        return max_rate;
      else
        return G_MAXUINT;
    }
    max_rate = MAX (max_rate, sender->receive_rate_history[i].rate);
  }

  return max_rate;
}

static void
add_to_receive_rate_history (TfrcSender *sender, guint receive_rate,
    guint64 now)
{
  int i;

  for (i = RECEIVE_RATE_HISTORY_SIZE - 1; i > 0; i --)
    sender->receive_rate_history[i] = sender->receive_rate_history[i-1];

  sender->receive_rate_history[0].rate = receive_rate;
  sender->receive_rate_history[0].timestamp = now;
}

static guint
maximize_receive_rate_history (TfrcSender *sender, guint receive_rate,
    guint64 now)
{
  guint max_rate;

  add_to_receive_rate_history (sender, receive_rate, now);

  max_rate = get_max_receive_rate (sender, TRUE);

  DEBUG_SENDER (sender, "MAXIMIZE recv: %u max: %u", receive_rate, max_rate);

  memset (sender->receive_rate_history, 0,
      sizeof(struct ReceiveRateItem) * RECEIVE_RATE_HISTORY_SIZE);

  sender->receive_rate_history[0].rate = max_rate;
  sender->receive_rate_history[0].timestamp = now;

  return max_rate;
}

static void
update_receive_rate_history (TfrcSender *sender, guint receive_rate,
    guint64 now)
{
  guint i;

  add_to_receive_rate_history (sender, receive_rate, now);

  for (i = 1; i < RECEIVE_RATE_HISTORY_SIZE; i++)
    if (sender->receive_rate_history[i].rate &&
        sender->receive_rate_history[i].timestamp <
        now - (2 * sender->averaged_rtt))
      sender->receive_rate_history[i].rate = 0;
}

const guint T_MBI = 64; /* the maximum backoff interval of 64 seconds */

static guint
compute_initial_rate (guint mss, guint rtt)
{
  if (G_UNLIKELY (rtt == 0))
    return 0;

  return (SECOND * MIN (4 * mss, MAX (2 * mss, 4380))) / rtt;
}

/* RFC 5348 section 4.3 step 4 second part */
static void
recompute_sending_rate (TfrcSender *sender, guint recv_limit,
    gdouble loss_event_rate, guint64 now)
{
  if (loss_event_rate > 0) {
    /* congestion avoidance phase */
    sender->computed_rate = calculate_bitrate (sender_get_segment_size (sender),
        sender->averaged_rtt, loss_event_rate);
    sender->rate = MAX (MIN (sender->computed_rate, recv_limit),
            sender_get_segment_size (sender)/T_MBI);
    DEBUG_SENDER (sender, "congestion avoidance: %u (computed: %u ss: %u)",
        sender->rate, sender->computed_rate, sender_get_segment_size (sender));
  } else if (now - sender->tld >= sender->averaged_rtt) {
    /* initial slow-start */
    sender->rate = MAX (MIN (2 * sender->rate, recv_limit),
        compute_initial_rate (sender->mss, sender->averaged_rtt));
    sender->tld = now;
    DEBUG_SENDER (sender, "initial slow start: %u", sender->rate);
  }
}

static void
tfrc_sender_update_inst_rate (TfrcSender *sender)
{
  if (!sender->last_sqrt_rtt)
    return;

  /*
   * Update the instantaneous
   *  transmit rate, X_inst, following RFC 5348 Section 4.5.
   */

  if (sender->sqmean_rtt)
    sender->sqmean_rtt = 0.9 * sender->sqmean_rtt + sender->last_sqrt_rtt / 10;
  else
    sender->sqmean_rtt = sender->last_sqrt_rtt;

  sender->inst_rate = sender->rate * sender->sqmean_rtt / sender->last_sqrt_rtt;
  if (sender->inst_rate < sender_get_segment_size (sender) / T_MBI)
    sender->inst_rate = sender_get_segment_size (sender) / T_MBI;

}

void
tfrc_sender_on_feedback_packet (TfrcSender *sender, guint64 now,
    guint rtt, guint receive_rate, gdouble loss_event_rate,
    gboolean is_data_limited)
{
  guint recv_limit; /* the limit on the sending rate computed from X_recv_set */

  g_return_if_fail (rtt > 0 && rtt <= 10 * SECOND);

  /* On first feedback packet, set he rate based on the mss and rtt */
  if (sender->tld == 0) {
    sender->rate = compute_initial_rate (sender->mss, rtt);
    sender->tld = now;
    DEBUG_SENDER (sender, "fb: initial rate: %u", sender->rate);
  }

  /* Apply the steps from RFC 5348 section 4.3 */

  /* Step 1 (calculating the rtt) is done before calling this function */

  /* Step 2: Update the RTT */
  if (sender->averaged_rtt == 0)
    sender->averaged_rtt = rtt;
  else
    sender->averaged_rtt = (sender->averaged_rtt * 9 + rtt) / 10;

  if (sender->averaged_rtt == 0)
    sender->averaged_rtt = 1;

  /* Step 3: Update the timeout interval */
  sender->retransmission_timeout = MAX (MAX (4 * sender->averaged_rtt,
          SECOND * 2 * sender_get_segment_size (sender) / sender->rate ),
      MIN_NOFEEDBACK_TIMER);

  /* Step 4: Update the allowed sending rate */


  if (G_UNLIKELY (is_data_limited)) {
    /* the entire interval covered by the feedback packet
       was a data-limited interval */
    if (loss_event_rate > sender->last_loss_event_rate) {
      guint i;
      /* the feedback packet reports a new loss event or an
         increase in the loss event rate p */

      /* Halve entries in the receive rate history */
      for (i = 0; i < RECEIVE_RATE_HISTORY_SIZE; i++)
        sender->receive_rate_history[i].rate /= 2;

      receive_rate *= 0.85;

      recv_limit = maximize_receive_rate_history (sender, receive_rate,
          now);
      DEBUG_SENDER (sender, "fb: data limited, new loss event %f > %f,"
          " recv_limit: %u", loss_event_rate, sender->last_loss_event_rate,
          recv_limit);
    } else {
      recv_limit = 2 * maximize_receive_rate_history (sender, receive_rate,
          now);
      DEBUG_SENDER (sender, "fb: data limited, no new loss event %f <= %f,"
          " recv_limit: %u", loss_event_rate, sender->last_loss_event_rate,
          recv_limit);
    }
  } else {
    /* typical behavior */
    update_receive_rate_history (sender, receive_rate, now);
    recv_limit = get_max_receive_rate (sender, FALSE);
    if (recv_limit < G_MAXUINT / 2)
      recv_limit *= 2;
    else
      recv_limit = G_MAXUINT;
    DEBUG_SENDER (sender, "fb: not data limited, recv_limit: %u",
        recv_limit);
  }

  recompute_sending_rate (sender, recv_limit, loss_event_rate, now);

  /* Step 5: update the instantaneous
     transmit rate, X_inst, following Section 4.5.
  */

  sender->last_sqrt_rtt = sqrt (rtt);
  tfrc_sender_update_inst_rate (sender);

  /* Step 6: Reset the nofeedback timer to expire after RTO seconds. */

  sender->nofeedback_timer_expiry = now + sender->retransmission_timeout;
  sender->sent_packet = FALSE;

  sender->last_loss_event_rate = loss_event_rate;
}

static void
update_limits(TfrcSender *sender, guint timer_limit, guint64 now)
{
  if (timer_limit < sender_get_segment_size (sender) / T_MBI)
    timer_limit = sender_get_segment_size (sender) / T_MBI;

  memset (sender->receive_rate_history, 0,
      sizeof(struct ReceiveRateItem) * RECEIVE_RATE_HISTORY_SIZE);

  sender->receive_rate_history[0].rate = timer_limit / 2;
  sender->receive_rate_history[0].timestamp = now;

  recompute_sending_rate (sender, timer_limit,
      sender->last_loss_event_rate, now);
}


void
tfrc_sender_no_feedback_timer_expired (TfrcSender *sender, guint64 now)
{
  guint receive_rate = get_max_receive_rate (sender, FALSE);
  guint recover_rate = compute_initial_rate (sender->mss, sender->averaged_rtt);

  if (sender->averaged_rtt == 0 && sender->sent_packet) {
    /* We do not have X_Bps or recover_rate yet.
     * Halve the allowed sending rate.
     */

    sender->rate = MAX ( sender->rate / 2,
        sender_get_segment_size (sender) / T_MBI);
    DEBUG_SENDER (sender, "no_fb: no p, initial, halve rate: %u", sender->rate);
    tfrc_sender_update_inst_rate (sender);
  } else if (((sender->last_loss_event_rate > 0 &&
              receive_rate < recover_rate) ||
          (sender->last_loss_event_rate == 0 &&
              sender->rate < 2 * recover_rate)) &&
      !sender->sent_packet) {
    /* Don't halve the allowed sending rate. */
    /* do nothing */
    DEBUG_SENDER (sender, "no_fb: idle, do nothing recv: %u recover: %u",
        receive_rate, recover_rate);
  } else if (sender->last_loss_event_rate == 0) {
    /* We do not have X_Bps yet.
     * Halve the allowed sending rate.
     */
    sender->rate = MAX ( sender->rate / 2,
        sender_get_segment_size (sender) / T_MBI);
    DEBUG_SENDER (sender, "no_fb: no p, halve rate: %u recover: %u, sent: %u", sender->rate,
        recover_rate, sender->sent_packet);
    tfrc_sender_update_inst_rate (sender);
  } else if (sender->computed_rate / 2 > receive_rate) {
    /* 2 * X_recv was already limiting the sending rate.
     * Halve the allowed sending rate.
   */
    DEBUG_SENDER (sender, "no_fb: computed rate %u > 2 * recv_rate %u",
        sender->computed_rate, receive_rate);
    update_limits(sender, receive_rate, now);
  } else {
    DEBUG_SENDER (sender, "no_fb: ELSE computed: %u", sender->computed_rate);
    update_limits(sender, sender->computed_rate / 2, now);
  }

  g_assert (sender->rate != 0);

  sender->nofeedback_timer_expiry = now + MAX (MAX ( 4 * sender->averaged_rtt,
          SECOND * 2 * sender_get_segment_size (sender) / sender->rate),
      MIN_NOFEEDBACK_TIMER);
  sender->sent_packet = FALSE;
}

void
tfrc_sender_sending_packet (TfrcSender *sender, guint size)
{
  /* this should be:
   * avg = size + (avg * 15/16)
   */
  sender->average_packet_size =
      size + ((15 * sender->average_packet_size) >> 4);

  sender->sent_packet = TRUE;
}

guint
tfrc_sender_get_send_rate (TfrcSender *sender)
{
  guint rate;

  if (!sender)
    return DEFAULT_MSS;

  if (sender->use_inst_rate && sender->inst_rate)
    rate = sender->inst_rate;
  else
    rate = sender->rate;

  if (sender->sp)
    return rate * (sender->average_packet_size >> 4) /
        ((sender->average_packet_size >> 4) + HEADER_SIZE);
  else
    return rate;
}

guint64
tfrc_sender_get_no_feedback_timer_expiry (TfrcSender *sender)
{
  return sender->nofeedback_timer_expiry;
}

guint
tfrc_sender_get_averaged_rtt (TfrcSender *sender)
{
  return sender->averaged_rtt;
}


#define NDUPACK 3 /* Number of packets to receive after a loss before declaring the loss event */
#define LOSS_EVENTS_MAX (9)
#define LOSS_INTERVALS_MAX (8)
#define MAX_HISTORY_SIZE (LOSS_EVENTS_MAX * 2) /* 2 is a random number */
#define MIN_HISTORY_DURATION (10)

typedef struct  {
  guint64 first_timestamp;
  guint first_seqnum;
  guint64 first_recvtime;

  guint64 last_timestamp;
  guint last_seqnum;
  guint64 last_recvtime;
} ReceivedInterval;

struct _TfrcReceiver {
  GQueue received_intervals;

  gboolean sp;

  guint sender_rtt;
  guint receive_rate;
  guint max_receive_rate;
  guint max_receive_rate_ss;
  guint64 feedback_timer_expiry;

  guint first_loss_interval;

  gdouble loss_event_rate;

  gboolean feedback_sent_on_last_timer;

  guint received_bytes;
  guint prev_received_bytes;
  guint64 received_bytes_reset_time;
  guint64 prev_received_bytes_reset_time;
  guint received_packets;
  guint prev_received_packets;
  guint sender_rtt_on_last_feedback;
};

TfrcReceiver *
tfrc_receiver_new (guint64 now)
{
  TfrcReceiver *receiver = g_slice_new0 (TfrcReceiver);

  g_queue_init (&receiver->received_intervals);
  receiver->received_bytes_reset_time = now;
  receiver->prev_received_bytes_reset_time = now;

  return receiver;
}


TfrcReceiver *
tfrc_receiver_new_sp (guint64 now)
{
  TfrcReceiver *receiver = tfrc_receiver_new (now);

  receiver->sp = TRUE;

  return receiver;
}

void
tfrc_receiver_free (TfrcReceiver *receiver)
{
  ReceivedInterval *ri;

  while ((ri = g_queue_pop_tail (&receiver->received_intervals)))
    g_slice_free (ReceivedInterval, ri);

  g_slice_free (TfrcReceiver, receiver);
}

/*
 * @s:  segment size in bytes
 * @R: RTT in milli seconds (instead of seconds)
 * @rate: the sending rate
 *
 * Returns the 1/p that would produce this sending rate
 * This is used to compute the first loss interval
 */

static gdouble
compute_first_loss_interval (gdouble s, gdouble R, gdouble rate)
{
  gdouble p_min = 0;
  gdouble p_max = 1;
  gdouble p;
  gdouble computed_rate;

  /* Use an iterative process to find p
   * it would be faster to use a table, but I'm lazy
   */

  do {
    p = (p_min + p_max) / 2;
    computed_rate = calculate_bitrate (s, R, p);

    if (computed_rate < rate)
      p_max = p;
    else
      p_min = p;

  } while (computed_rate < 0.95 * rate || computed_rate > 1.05 * rate);

  return 1 / p;
}


/* Implements RFC 5348 section 5 */
static gdouble
calculate_loss_event_rate (TfrcReceiver *receiver, guint64 now)
{
  guint64 loss_event_times[LOSS_EVENTS_MAX];
  guint loss_event_seqnums[LOSS_EVENTS_MAX];
  guint loss_event_pktcount[LOSS_EVENTS_MAX];
  guint loss_intervals[LOSS_EVENTS_MAX];
  const gdouble weights[8] = { 1.0, 1.0, 1.0, 1.0, 0.8, 0.6, 0.4, 0.2 };
  gint max_index = -1;
  GList *item;
  guint max_seqnum = 0;
  gint i;
  guint max_interval;
  gdouble I_tot0 = 0;
  gdouble I_tot1 = 0;
  gdouble W_tot = 0;
  gdouble I_tot;

  if (receiver->sender_rtt == 0)
    return 0;

  if (receiver->received_intervals.length < 2)
    return 0;

  DEBUG_RECEIVER (receiver, "start loss event rate computation (rtt: %u)",
      receiver->sender_rtt);

  for (item = g_queue_peek_head_link (&receiver->received_intervals)->next;
       item;
       item = item->next) {
    ReceivedInterval *current = item->data;
    ReceivedInterval *prev = item->prev->data;
    guint64 start_ts;
    guint start_seqnum;

    max_seqnum = current->last_seqnum;

    DEBUG_RECEIVER (receiver, "Loss: ts %"G_GUINT64_FORMAT
        "->%"G_GUINT64_FORMAT" seq %u->%u",
        prev->last_timestamp, current->first_timestamp, prev->last_seqnum,
        current->first_seqnum);

    /* If the current loss is entirely within one RTT of the beginning of the
     * last loss, lets merge it into there
     */
    if (max_index >= 0 && current->first_timestamp <
        loss_event_times[max_index % LOSS_EVENTS_MAX] + receiver->sender_rtt) {
      loss_event_pktcount[max_index % LOSS_EVENTS_MAX] +=
          current->first_seqnum - prev->last_seqnum;
      DEBUG_RECEIVER (receiver, "Merged: pktcount[%u] = %u", max_index,
          loss_event_pktcount[max_index % LOSS_EVENTS_MAX]);
      continue;
    }

    if (max_index >= 0 && prev->last_timestamp <
        loss_event_times[max_index % LOSS_EVENTS_MAX] + receiver->sender_rtt) {
      /* This is the case where a loss event ends in a middle of an interval
       * without packets, then we close this loss event and start a new one
       */
      start_ts = loss_event_times[max_index % LOSS_EVENTS_MAX] +
          receiver->sender_rtt;
      start_seqnum = prev->last_seqnum +
          gst_util_uint64_scale_round (
            current->first_seqnum - prev->last_seqnum,
            start_ts - prev->last_timestamp,
            1 + current->first_timestamp - prev->last_timestamp);
      loss_event_pktcount[max_index % LOSS_EVENTS_MAX] +=
          start_seqnum - prev->last_seqnum - 1;
      DEBUG_RECEIVER (receiver,
          "Loss ends inside loss interval pktcount[%u] = %u",
          max_index, loss_event_pktcount[max_index % LOSS_EVENTS_MAX]);
    } else {
      /* this is the case where the packet loss starts an entirely new loss
       * event
       */
      start_ts = prev->last_timestamp +
          gst_util_uint64_scale_round (1,
              current->first_timestamp - prev->last_timestamp,
              current->first_seqnum - prev->last_seqnum);
      start_seqnum = prev->last_seqnum + 1;
    }

    DEBUG_RECEIVER (receiver, "start_ts: %" G_GUINT64_FORMAT " seqnum: %u",
        start_ts, start_seqnum);

    /* Now we have one or more loss events that start
     * during this interval of lost packets, if there is more than one
     * all but the last one are of RTT length
     */
    while (start_ts <= current->first_timestamp) {
      max_index ++;

      loss_event_times[max_index % LOSS_EVENTS_MAX] = start_ts;
      loss_event_seqnums[max_index % LOSS_EVENTS_MAX] = start_seqnum;
      if (current->first_timestamp == prev->last_timestamp) {
        /* if current->first_ts == prev->last_ts,
         * then the computation of start_seqnum below will yield a division
         * by 0
         */
        loss_event_pktcount[max_index % LOSS_EVENTS_MAX] =
          current->first_seqnum - start_seqnum;
        break;
      }

      start_ts += receiver->sender_rtt;
      start_seqnum = prev->last_seqnum +
          gst_util_uint64_scale_round (
            current->first_seqnum - prev->last_seqnum,
            start_ts - prev->last_timestamp,
            current->first_timestamp - prev->last_timestamp);

      /* Make sure our interval has at least one packet in it */
      if (G_UNLIKELY (start_seqnum <=
              loss_event_seqnums[max_index % LOSS_EVENTS_MAX]))
      {
        start_seqnum = loss_event_seqnums[max_index % LOSS_EVENTS_MAX] + 1;
        start_ts = prev->last_timestamp +
            gst_util_uint64_scale_round (
              current->first_timestamp - prev->last_timestamp,
              start_seqnum - prev->last_seqnum,
              current->first_seqnum - prev->last_seqnum);
      }

      if (start_seqnum > current->first_seqnum)
      {
        g_assert (start_ts > current->first_timestamp);
        start_seqnum = current->first_seqnum;
        /* No need top change start_ts, the loop will stop anyway */
      }
      loss_event_pktcount[max_index % LOSS_EVENTS_MAX] = start_seqnum -
          loss_event_seqnums[max_index % LOSS_EVENTS_MAX];
      DEBUG_RECEIVER (receiver, "loss %u times: %" G_GUINT64_FORMAT
          " seqnum: %u pktcount: %u",
          max_index, loss_event_times[max_index % LOSS_EVENTS_MAX],
          loss_event_seqnums[max_index % LOSS_EVENTS_MAX],
          loss_event_pktcount[max_index % LOSS_EVENTS_MAX]);
    }
  }

  if (max_index < 0 ||
      (max_index < 1 && receiver->max_receive_rate == 0))
    return 0;

  /* RFC 5348 Section 5.3: The size of loss events */
  loss_intervals[0] =
    max_seqnum - loss_event_seqnums[max_index % LOSS_EVENTS_MAX] + 1;
  DEBUG_RECEIVER (receiver, "intervals[0] = %u", loss_intervals[0]);
  for (i = max_index - 1, max_interval = 1;
       max_interval < LOSS_INTERVALS_MAX &&
         i >= 0 && i > max_index - LOSS_EVENTS_MAX;
       i--, max_interval++) {
    guint cur_i = i % LOSS_EVENTS_MAX;
    guint prev_i = (i + 1) % LOSS_EVENTS_MAX;

    /* if its Small Packet variant and the loss event is short,
     * that is less than 2 * RTT, then the loss interval is divided by the
     * number of packets lost
     * see RFC 4828 section 3 bullet 3 paragraph 2 */
    if (receiver->sp &&
        loss_event_times[prev_i] - loss_event_times[cur_i] <
        2 * receiver->sender_rtt)
      loss_intervals[max_interval] = (loss_event_seqnums[prev_i] -
          loss_event_seqnums[cur_i]) / loss_event_pktcount[cur_i];
    else
      loss_intervals[max_interval] =
        loss_event_seqnums[prev_i] - loss_event_seqnums[cur_i];
    DEBUG_RECEIVER (receiver, "intervals[%u] = %u", max_interval,
        loss_intervals[max_interval]);
  }

  /* If the first loss interval is still used, use the computed
   * value according to RFC 5348 section 6.3.1
   */
  if (max_interval < LOSS_INTERVALS_MAX)
  {
    if (!receiver->first_loss_interval)
    {
      receiver->first_loss_interval =
          /* segment size is 1 because we're computing it based on the
           * X_pps equation.. in packets/s
           */
          compute_first_loss_interval (receiver->max_receive_rate_ss,
              receiver->sender_rtt, receiver->max_receive_rate);
      DEBUG_RECEIVER (receiver, "Computed the first loss interval to %u"
          " (rtt: %u s: %u rate:%u)",
          receiver->first_loss_interval, receiver->sender_rtt,
          receiver->max_receive_rate_ss, receiver->max_receive_rate);
    }
    loss_intervals[max_interval] = receiver->first_loss_interval;
    DEBUG_RECEIVER (receiver, "intervals[%u] = %u", max_interval,
        loss_intervals[max_interval]);
    max_interval++;
 }

  /* Section 5.4: Average loss rate */
  for (i = 1; i < max_interval; i++) {
    I_tot1 += loss_intervals[i] * weights[i - 1];
    W_tot += weights[i - 1];
  }

  /* Modified according to RFC 4828 section 3 bullet 3 paragraph 4*/
  if (receiver->sp && now - loss_event_times[0] < 2 * receiver->sender_rtt) {
    I_tot = I_tot1;
  } else {
    for (i = 0; i < max_interval - 1; i++)
      I_tot0 += loss_intervals[i] * weights[i];

    I_tot = MAX (I_tot0, I_tot1);
  }

  return W_tot / I_tot;
}


/* Implements RFC 5348 section 6.1 */
gboolean
tfrc_receiver_got_packet (TfrcReceiver *receiver, guint64 timestamp,
    guint64 now, guint seqnum, guint sender_rtt, guint packet_size)
{
  GList *item = NULL;
  ReceivedInterval *current = NULL;
  ReceivedInterval *prev = NULL;
  gboolean recalculate_loss_rate = FALSE;
  gboolean retval = FALSE;
  gboolean history_too_short = !sender_rtt; /* No RTT, keep all history */

  receiver->received_bytes += packet_size;
  receiver->received_packets++;

  g_assert (sender_rtt != 0 || receiver->sender_rtt == 0);

  if (receiver->sender_rtt)
    receiver->sender_rtt = (0.9 * receiver->sender_rtt) + (sender_rtt / 10);
  else
    receiver->sender_rtt = sender_rtt;

  /* RFC 5348 section 6.3: First packet received */
  if (g_queue_get_length (&receiver->received_intervals) == 0 ||
      receiver->sender_rtt == 0) {
    if (receiver->sender_rtt)
      receiver->feedback_timer_expiry = now + receiver->sender_rtt;

    /* First packet or no RTT yet, lets send a feedback packet */
    retval = TRUE;
  }

  /* RFC 5348 section 6.1 Step 1: Add to packet history */

  for (item = g_queue_peek_tail_link (&receiver->received_intervals);
       item;
       item = item->prev) {
    current = item->data;
    prev = item->prev ? item->prev->data : NULL;

    if (G_LIKELY (seqnum == current->last_seqnum + 1)) {
      /* Extend the current packet forwardd */
      current->last_seqnum = seqnum;
      current->last_timestamp = timestamp;
      current->last_recvtime = now;
    } else if (seqnum >= current->first_seqnum &&
        seqnum <= current->last_seqnum) {
      /* Is inside the current interval, must be duplicate, ignore */
    } else if (seqnum > current->last_seqnum + 1) {
      /* We had a loss, lets add a new one */
      prev = current;

      current = g_slice_new (ReceivedInterval);
      current->first_timestamp = current->last_timestamp = timestamp;
      current->first_seqnum = current->last_seqnum = seqnum;
      current->first_recvtime = current->last_recvtime = now;
      g_queue_push_tail (&receiver->received_intervals, current);

      item = g_queue_peek_tail_link (&receiver->received_intervals);
    } else if (seqnum == current->first_seqnum - 1) {
      /* Extend the current packet backwards */
      current->first_seqnum = seqnum;
      current->first_timestamp = timestamp;
      current->first_recvtime = now;
    } else if (seqnum < current->first_timestamp &&
        (!prev || seqnum > prev->last_seqnum + 1)) {
      /* We have something that goes in the middle of a gap,
         so lets created a new received interval */
      current = g_slice_new (ReceivedInterval);

      current->first_timestamp = current->last_timestamp = timestamp;
      current->first_seqnum = current->last_seqnum = seqnum;
      current->first_recvtime = current->last_recvtime = now;

      g_queue_insert_before (&receiver->received_intervals, item, current);
      item = item->prev;
      prev = item->prev ? item->prev->data : NULL;
    } else
      continue;
    break;
  }

  /* Don't forget history if we have aless than MIN_HISTORY_DURATION * rtt
   * of history
   */
  if (!history_too_short)
  {
    ReceivedInterval *newest =
      g_queue_peek_tail (&receiver->received_intervals);
    ReceivedInterval *oldest =
      g_queue_peek_head (&receiver->received_intervals);
    if (newest && oldest)
      history_too_short =
        newest->last_timestamp - oldest->first_timestamp <
        MIN_HISTORY_DURATION * receiver->sender_rtt;
    else
      history_too_short = TRUE;
  }

  /* It's the first one or we're at the start */
  if (G_UNLIKELY (!current)) {
    /* If its before MAX_HISTORY_SIZE, its too old, just discard it */
    if (!history_too_short &&
        g_queue_get_length (&receiver->received_intervals) > MAX_HISTORY_SIZE)
      return retval;

    current = g_slice_new (ReceivedInterval);

    current->first_timestamp = current->last_timestamp = timestamp;
    current->first_seqnum = current->last_seqnum = seqnum;
    current->first_recvtime = current->last_recvtime = now;
    g_queue_push_head (&receiver->received_intervals, current);
  }

  if (!history_too_short &&
      g_queue_get_length (&receiver->received_intervals) > MAX_HISTORY_SIZE) {
    ReceivedInterval *remove = g_queue_pop_head (&receiver->received_intervals);
    if (remove == prev)
      prev = NULL;
    g_slice_free (ReceivedInterval, remove);
  }


  if (prev && (current->last_seqnum - current->first_seqnum == NDUPACK))
    recalculate_loss_rate = TRUE;


  if (prev &&  G_UNLIKELY (prev->last_seqnum + 1 == current->first_seqnum)) {
    /* Merge closed gap if any */
    current->first_seqnum = prev->first_seqnum;
    current->first_timestamp = prev->first_timestamp;
    current->first_recvtime = prev->first_recvtime;

    g_slice_free (ReceivedInterval, prev);
    g_queue_delete_link (&receiver->received_intervals, item->prev);

    prev = item->prev ? item->prev->data : NULL;

    recalculate_loss_rate = TRUE;
  }

  /* RFC 5348 section 6.1 Step 2, 3, 4:
   * Check if done
   * If not done, recalculte the loss event rate,
   * and possibly send a feedback message
   */

  if (receiver->sender_rtt &&
      (recalculate_loss_rate || !receiver->feedback_sent_on_last_timer)) {
    gdouble new_loss_event_rate = calculate_loss_event_rate (receiver, now);

    if (new_loss_event_rate > receiver->loss_event_rate ||
        !receiver->feedback_sent_on_last_timer)
      retval |= tfrc_receiver_feedback_timer_expired (receiver, now);
  }

  return retval;
}

gboolean
tfrc_receiver_feedback_timer_expired (TfrcReceiver *receiver, guint64 now)
{
  if (receiver->received_bytes == 0 ||
      receiver->prev_received_bytes_reset_time == now) {
    g_assert (receiver->sender_rtt != 0);
    receiver->feedback_timer_expiry = now + receiver->sender_rtt;
    receiver->feedback_sent_on_last_timer = FALSE;
    return FALSE;
  }
  else
  {
    return TRUE;
  }
}

gboolean
tfrc_receiver_send_feedback (TfrcReceiver *receiver, guint64 now,
    double *loss_event_rate, guint *receive_rate)
{
  guint received_bytes;
  guint64 received_bytes_reset_time;
  guint received_packets;

  if (now == receiver->prev_received_bytes_reset_time)
    return FALSE;

  if (now - receiver->received_bytes_reset_time >
      receiver->sender_rtt_on_last_feedback ) {
    receiver->prev_received_bytes_reset_time =
        receiver->received_bytes_reset_time;
    receiver->prev_received_bytes = receiver->received_bytes;
    receiver->prev_received_packets = receiver->received_packets;
    received_bytes = receiver->received_bytes;
    received_packets = receiver->received_packets;
    received_bytes_reset_time = receiver->received_bytes_reset_time;
  } else {
    receiver->prev_received_bytes += receiver->received_bytes;
    receiver->prev_received_packets += receiver->received_packets;
    received_bytes = receiver->prev_received_bytes;
    received_packets = receiver->prev_received_packets;
    received_bytes_reset_time = receiver->prev_received_bytes_reset_time;
  }

  receiver->received_bytes_reset_time = now;
  receiver->received_bytes = 0;
  receiver->received_packets = 0;

  receiver->receive_rate = gst_util_uint64_scale_round (SECOND, received_bytes,
      now - received_bytes_reset_time);

  if (receiver->sender_rtt_on_last_feedback &&
      receiver->receive_rate > receiver->max_receive_rate)
  {
    receiver->max_receive_rate = receiver->receive_rate;
    receiver->max_receive_rate_ss = received_bytes / received_packets;
  }

  receiver->loss_event_rate = calculate_loss_event_rate (receiver, now);

  if (receiver->sender_rtt)
    receiver->feedback_timer_expiry = now + receiver->sender_rtt;
  receiver->sender_rtt_on_last_feedback = receiver->sender_rtt;
  receiver->feedback_sent_on_last_timer = TRUE;

  DEBUG_RECEIVER (receiver, "P: %f recv_rate: %u", receiver->loss_event_rate,
      receiver->receive_rate);

  *receive_rate = receiver->receive_rate;
  *loss_event_rate = receiver->loss_event_rate;

  return TRUE;
}

guint64
tfrc_receiver_get_feedback_timer_expiry (TfrcReceiver *receiver)
{
  g_assert (receiver->sender_rtt || receiver->feedback_timer_expiry == 0);
  return receiver->feedback_timer_expiry;
}

struct _TfrcIsDataLimited
{
  guint64 not_limited_1;
  guint64 not_limited_2;
  guint64 t_new;
  guint64 t_next;
};

/*
 * This implements the algorithm proposed in RFC 5248 section 8.2.1 */

TfrcIsDataLimited *
tfrc_is_data_limited_new (guint64 now)
{
  TfrcIsDataLimited *idl = g_slice_new0 (TfrcIsDataLimited);

  return idl;
}

void
tfrc_is_data_limited_free (TfrcIsDataLimited *idl)
{
  g_slice_free (TfrcIsDataLimited, idl);
}

void
tfrc_is_data_limited_not_limited_now (TfrcIsDataLimited *idl, guint64 now)
{
  /* Sender is not data-limited at this instant. */
  if (idl->not_limited_1 <= idl->t_new)
    /* Goal: NotLimited1 > t_new. */
    idl->not_limited_1 = now;
  else if (idl->not_limited_2 <= idl->t_next)
    /* Goal: NotLimited2 > t_next. */
    idl->not_limited_2 = now;
}

/*
 * Returns TRUE if the period since the previous feedback packet
 * was data limited
 */
gboolean
tfrc_is_data_limited_received_feedback (TfrcIsDataLimited *idl, guint64 now,
    guint64 last_packet_timestamp, guint rtt)
{
  gboolean ret;
  guint64 t_old;

  idl->t_new = last_packet_timestamp;
  t_old = idl->t_new - rtt;
  idl->t_next = now;
  if ((t_old < idl->not_limited_1 && idl->not_limited_1 <= idl->t_new) ||
      (t_old <  idl->not_limited_2 && idl->not_limited_2 <= idl->t_new))
    /* This was NOT a data-limited interval */
    ret =  FALSE;
  else
    /* This was a data-limited interval. */
    ret = TRUE;

  if (idl->not_limited_1 <= idl->t_new && idl->not_limited_2 > idl->t_new)
      idl->not_limited_1 = idl->not_limited_2;

  return ret;
}
