/* Farstream unit tests for fsrawconference
 *
 * Copyright (C) 2007,2010 Collabora, Nokia
 * @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * @author: Mike Ruprecht <mike.ruprecht@collabora.co.uk>
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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <gst/audio/audio.h>
#include <gst/check/gstcheck.h>
#include <farstream/fs-conference.h>
#include <farstream/fs-stream-transmitter.h>

#include "check-threadsafe.h"

#include "testutils.h"

struct SimpleTestConference {
  gint id;
  gchar *cname;

  GstElement *pipeline;
  GstElement *conference;
  FsSession *session;
  GstElement *fakesrc;

  gboolean started;

  gboolean destroyed;

  GList *streams;
};


struct SimpleTestStream {
  struct SimpleTestConference *dat;
  struct SimpleTestConference *target;

  FsParticipant *participant;
  FsStream *stream;

  gboolean destroyed;

  gchar *transmitter;

  gint buffer_count;

  GCallback handoff_handler;

  gboolean got_candidates;

  gint flags;
};

static struct SimpleTestStream *
find_pointback_stream (
    struct SimpleTestConference *dat,
    struct SimpleTestConference *target);


struct SimpleTestConference **dats;
GMainLoop *loop;
int count = 0;

#define WAITING_ON_LAST_CODEC   (1<<0)
#define SHOULD_BE_LAST_CODEC    (1<<1)
#define HAS_BEEN_RESET          (1<<2)

gint max_buffer_count = 20;

guint max_src_pads = 1;

GMutex testlock;

#define TEST_LOCK()   g_mutex_lock (&testlock)
#define TEST_UNLOCK() g_mutex_unlock (&testlock)

gboolean select_last_codec = FALSE;
gboolean reset_to_last_codec = FALSE;

static GstBusSyncReply
default_sync_handler (GstBus *bus, GstMessage *message, gpointer data)
{
  struct SimpleTestConference *dat = data;
  GList *codecs = NULL;

  /* Get the codecs property which should take the session lock to make sure
   * it is not held across signal emissions
   */
  if (dat->session)
    g_object_get (dat->session, "codecs", &codecs, NULL);
  fs_codec_list_destroy (codecs);

  return GST_BUS_PASS;
}

struct SimpleTestConference *
setup_simple_conference_full (
    gint id,
    gchar *conference_elem,
    gchar *cname,
    FsMediaType mediatype)
{
  struct SimpleTestConference *dat = g_new0 (struct SimpleTestConference, 1);
  GError *error = NULL;
  guint tos;
  GstBus *bus;

  dat->id = id;

  dat->pipeline = gst_pipeline_new ("pipeline");
  fail_if (dat->pipeline == NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE (dat->pipeline));
  fail_if (bus == NULL);
  gst_bus_set_sync_handler (bus, default_sync_handler, dat, NULL);
  gst_object_unref (bus);

  dat->conference = gst_element_factory_make (conference_elem, NULL);
  fail_if (dat->conference == NULL, "Could not build %s", conference_elem);
  fail_unless (gst_bin_add (GST_BIN (dat->pipeline), dat->conference),
      "Could not add conference to the pipeline");

  dat->session = fs_conference_new_session (FS_CONFERENCE (dat->conference),
      mediatype, &error);
  if (error)
    fail ("Error while creating new session (%d): %s",
        error->code, error->message);
  fail_if (dat->session == NULL, "Could not make session, but no GError!");

  g_object_set (dat->session, "tos", 2, NULL);
  g_object_get (dat->session, "tos", &tos, NULL);
  fail_unless (tos == 2);


  g_object_set_data (G_OBJECT (dat->conference), "dat", dat);

  return dat;
}


struct SimpleTestConference *
setup_simple_conference (
    gint id,
    gchar *conference_elem,
    gchar *cname)
{
  return setup_simple_conference_full (id, conference_elem, cname,
      FS_MEDIA_TYPE_AUDIO);
}

struct SimpleTestStream *
simple_conference_add_stream (
    struct SimpleTestConference *dat,
    struct SimpleTestConference *target,
    const gchar *transmitter,
    guint st_param_count,
    GParameter *st_params)
{
  struct SimpleTestStream *st = g_new0 (struct SimpleTestStream, 1);
  GError *error = NULL;

  st->dat = dat;
  st->target = target;
  st->transmitter = g_strdup (transmitter);

  st->participant = fs_conference_new_participant (
      FS_CONFERENCE (dat->conference), &error);
  if (error)
    fail ("Error while creating new participant (%d): %s",
        error->code, error->message);
  fail_if (st->participant == NULL, "Could not make participant, but no GError!");

  st->stream = fs_session_new_stream (dat->session, st->participant,
      FS_DIRECTION_BOTH, &error);
  if (error)
    fail ("Error while creating new stream (%d): %s",
        error->code, error->message);
  fail_if (st->stream == NULL, "Could not make stream, but no GError!");

  fail_unless (fs_stream_set_transmitter (st->stream, transmitter, st_params,
          st_param_count, &error));
  fail_unless (error == NULL);


  g_object_set_data (G_OBJECT (st->stream), "SimpleTestStream", st);

  dat->streams = g_list_append (dat->streams, st);

  return st;
}


void
cleanup_simple_stream (struct SimpleTestStream *st)
{
  if (st->stream)
  {
    fs_stream_destroy (st->stream);
    g_object_unref (st->stream);
  }
  g_object_unref (st->participant);
  g_free (st->transmitter);
  g_free (st);
}

void
cleanup_simple_conference (struct SimpleTestConference *dat)
{

  g_list_foreach (dat->streams, (GFunc) cleanup_simple_stream, NULL);
  g_list_free (dat->streams);

  if (dat->session)
  {
    fs_session_destroy (dat->session);
    g_object_unref (dat->session);
  }
  gst_object_unref (dat->pipeline);
  g_free (dat);
}


void
setup_fakesrc (struct SimpleTestConference *dat)
{
  GstPad *sinkpad = NULL, *srcpad = NULL;

  GST_DEBUG ("Adding fakesrc");


  g_object_get (dat->session, "sink-pad", &sinkpad, NULL);
  fail_if (sinkpad == NULL, "Could not get session sinkpad");

  dat->fakesrc = gst_element_factory_make ("audiotestsrc", NULL);
  fail_if (dat->fakesrc == NULL, "Could not make audiotestsrc");
  gst_bin_add (GST_BIN (dat->pipeline), dat->fakesrc);

  g_object_set (dat->fakesrc,
      "blocksize", 10,
      "is-live", TRUE,
      "volume", 0.3,
      NULL);

  srcpad = gst_element_get_static_pad (dat->fakesrc, "src");

  fail_unless (gst_pad_link (srcpad, sinkpad) == GST_PAD_LINK_OK,
      "Could not link the capsfilter and the fsrawconference");

  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  if (dat->started)
    fail_if (gst_element_set_state (dat->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE);
}

static gboolean
pad_count_fold (const GValue *item, GValue *val, gpointer user_data)
{
  g_value_set_uint (val, g_value_get_uint (val) + 1);

  return TRUE;
}

guint
count_stream_pads (FsStream *stream)
{
  GstIterator *iter = fs_stream_iterate_src_pads (stream);
  guint count = 0;

  fail_if (iter == NULL);

  for (;;)
  {
    GstIteratorResult res;
    GValue val = {0};

    g_value_init (&val, G_TYPE_UINT);

    res = gst_iterator_fold (iter, pad_count_fold, &val, NULL);

    fail_if (res == GST_ITERATOR_ERROR);

    if (res != GST_ITERATOR_RESYNC)
    {
      count = g_value_get_uint (&val);
      break;
    }

    gst_iterator_resync (iter);
  }

  gst_iterator_free (iter);

  return count;
}

GST_START_TEST (test_rawconference_new)
{
  struct SimpleTestConference *dat = NULL;
  struct SimpleTestStream *st = NULL;
  guint id = 999;
  GList *codecs = NULL;
  FsMediaType media_type;
  GstPad *sinkpad = NULL;
  gchar *str = NULL;
  GstElement *conf = NULL;
  FsSession *sess = NULL;
  FsParticipant *part = NULL;
  FsStreamDirection dir;

  dat = setup_simple_conference (1, "fsrawconference", "bob@127.0.0.1");
  //st = simple_conference_add_stream (dat, dat, "shm", 0, NULL);
  st = simple_conference_add_stream (dat, dat, "rawudp", 0, NULL);

  g_object_get (dat->session,
      "id", &id,
      "codecs", &codecs,
      "media-type", &media_type,
      "sink-pad", &sinkpad,
      "conference", &conf,
      NULL);

  ts_fail_unless (id == 1, "The id of the first session should be 1 not %d",
      id);
  ts_fail_unless (codecs == NULL, "Codecs should be NULL");
  fs_codec_list_destroy (codecs);
  ts_fail_unless (media_type == FS_MEDIA_TYPE_AUDIO, "Media type isnt audio,"
      " its %d", media_type);
  ts_fail_if (sinkpad == NULL, "Sink pad should not be null");
  str = g_strdup_printf ("sink_%d", id);
  ts_fail_unless (!strcmp (str, GST_OBJECT_NAME (sinkpad)), "Sink pad is %s"
      " instead of being %d", GST_OBJECT_NAME (sinkpad), str);
  gst_object_unref (sinkpad);
  g_free (str);
  ts_fail_unless (conf == dat->conference, "Conference pointer from the session"
      " is wrong");
  gst_object_unref (conf);


  g_object_get (st->stream,
      "participant", &part,
      "session", &sess,
      "direction", &dir,
      NULL);
  ts_fail_unless (part == st->participant, "The stream does not have the right"
      " participant");
  g_object_unref (part);
  ts_fail_unless (sess == dat->session, "The stream does not have the right"
      " session");
  g_object_unref (sess);
  ts_fail_unless (dir == FS_DIRECTION_BOTH, "The direction is not both");

  ts_fail_unless (count_stream_pads (st->stream) == 0);
  g_object_set (st->stream, "direction", FS_DIRECTION_NONE, NULL);
  g_object_get (st->stream, "direction", &dir, NULL);
  ts_fail_unless (dir == FS_DIRECTION_NONE, "The direction is not both");

  cleanup_simple_conference (dat);
}
GST_END_TEST;


static void
_new_local_candidate (FsStream *stream, FsCandidate *candidate)
{
  struct SimpleTestStream *st = g_object_get_data (G_OBJECT (stream),
      "SimpleTestStream");
  gboolean ret;
  GError *error = NULL;
  struct SimpleTestStream *other_st;
  GList *candidates = NULL;
  FsCandidate *freecand = NULL;

  TEST_LOCK ();

  if (st->destroyed)
  {
    TEST_UNLOCK ();
    return;
  }

  other_st = find_pointback_stream (st->target, st->dat);
  if (other_st->destroyed)
  {
    TEST_UNLOCK ();
    return;
  }

  st->got_candidates = TRUE;

  GST_DEBUG ("%d:%d: Setting remote candidate for component %d",
      other_st->dat->id,
      other_st->target->id,
      candidate->component_id);

  if (!strcmp ("shm", st->transmitter))
  {
    freecand = fs_candidate_new (NULL, candidate->component_id,
        candidate->type, FS_NETWORK_PROTOCOL_UDP, NULL, 0);
    freecand->username = g_strdup (candidate->ip);
    candidates = g_list_prepend (NULL, freecand);
  }
  else
  {
    candidates = g_list_prepend (NULL, candidate);
  }

  ret = fs_stream_force_remote_candidates (other_st->stream, candidates,
      &error);
  g_list_free (candidates);
  fs_candidate_destroy (freecand);

  if (error)
    ts_fail ("Error while adding candidate: (%s:%d) %s",
      g_quark_to_string (error->domain), error->code, error->message);

  ts_fail_unless (ret == TRUE, "No detailed error from add_remote_candidate");

  TEST_UNLOCK ();
}

static void
_current_send_codec_changed (FsSession *session, FsCodec *codec)
{
  struct SimpleTestConference *dat = NULL;
  FsConference *conf = NULL;
  gchar *str = NULL;

  g_object_get (session, "conference", &conf, NULL);
  dat = g_object_get_data (G_OBJECT (conf), "dat");
  gst_object_unref (conf);

  str = fs_codec_to_string (codec);
  GST_DEBUG ("%d: New send codec: %s", dat->id, str);
  g_free (str);
}

static void
_local_candidates_prepared (FsStream *stream)
{
  struct SimpleTestStream *st = g_object_get_data (G_OBJECT (stream),
      "SimpleTestStream");

  if (!st->got_candidates)
  {
    g_debug ("Skipping test because there are no candidates");
    g_main_loop_quit (loop);
  }

}


static gboolean
_bus_callback (GstBus *bus, GstMessage *message, gpointer user_data)
{
  struct SimpleTestConference *dat = user_data;

  switch (GST_MESSAGE_TYPE (message))
  {
    case GST_MESSAGE_ELEMENT:
      {
        const GstStructure *s = gst_message_get_structure (message);
        ts_fail_if (s==NULL, "NULL structure in element message");
        if (gst_structure_has_name (s, "farstream-error"))
        {
          const GValue *value;
          FsError errorno;
          const gchar *error;

          ts_fail_unless (
            FS_IS_CONFERENCE (GST_MESSAGE_SRC (message)),
            "Received farstream-error from non-farstream element");

          ts_fail_unless (
              gst_structure_has_field_typed (s, "src-object", G_TYPE_OBJECT),
              "farstream-error structure has no src-object field");
          ts_fail_unless (
              gst_structure_has_field_typed (s, "error-no", FS_TYPE_ERROR),
              "farstream-error structure has no src-object field");
          ts_fail_unless (
              gst_structure_has_field_typed (s, "error-msg", G_TYPE_STRING),
              "farstream-error structure has no error-msg field");

          value = gst_structure_get_value (s, "error-no");
          errorno = g_value_get_enum (value);
          error = gst_structure_get_string (s, "error-msg");

          ts_fail ("Error on BUS (%d) %s", errorno, error);
        }
        else if (gst_structure_has_name (s, "farstream-new-local-candidate"))
        {
          FsStream *stream;
          FsCandidate *candidate;
          const GValue *value;

          ts_fail_unless (
            FS_IS_CONFERENCE (GST_MESSAGE_SRC (message)),
            "Received farstream-error from non-farstream element");

          ts_fail_unless (
              gst_structure_has_field_typed (s, "stream", FS_TYPE_STREAM),
              "farstream-new-local-candidate structure has no stream field");
          ts_fail_unless (
              gst_structure_has_field_typed (s, "candidate", FS_TYPE_CANDIDATE),
              "farstream-new-local-candidate structure has no candidate field");

          value = gst_structure_get_value (s, "stream");
          stream = g_value_get_object (value);

          value = gst_structure_get_value (s, "candidate");
          candidate = g_value_get_boxed (value);

          ts_fail_unless (stream && candidate, "new-local-candidate with NULL"
              " stream(%p) or candidate(%p)", stream, candidate);

          _new_local_candidate (stream, candidate);
        }
        else if (gst_structure_has_name (s,
                "farstream-new-active-candidate-pair"))
        {
          FsStream *stream;
          FsCandidate *local_candidate, *remote_candidate;
          const GValue *value;

          ts_fail_unless (
            FS_IS_CONFERENCE (GST_MESSAGE_SRC (message)),
            "Received farstream-error from non-farstream element");

          ts_fail_unless (
              gst_structure_has_field_typed (s, "stream", FS_TYPE_STREAM),
              "farstream-new-active-candidate-pair structure"
              " has no stream field");
          ts_fail_unless (
              gst_structure_has_field_typed (s, "local-candidate",
                  FS_TYPE_CANDIDATE),
              "farstream-new-active-candidate-pair structure"
              " has no local-candidate field");
          ts_fail_unless (
              gst_structure_has_field_typed (s, "remote-candidate",
                  FS_TYPE_CANDIDATE),
              "farstream-new-active-candidate-pair structure"
              " has no remote-candidate field");

          value = gst_structure_get_value (s, "stream");
          stream = g_value_get_object (value);
          value = gst_structure_get_value (s, "local-candidate");
          local_candidate = g_value_get_boxed (value);
          value = gst_structure_get_value (s, "remote-candidate");
          remote_candidate = g_value_get_boxed (value);

          ts_fail_unless (stream && local_candidate && remote_candidate,
              "new-local-candidate with NULL stream(%p)"
              " or local_candidate(%p) or remote_candidate(%p)",
              stream, local_candidate, remote_candidate);
        }
        else if (gst_structure_has_name (s,
                "farstream-current-send-codec-changed"))
        {
          FsSession *session;
          FsCodec *codec;
          const GValue *value;

          ts_fail_unless (
            FS_IS_CONFERENCE (GST_MESSAGE_SRC (message)),
            "Received farstream-error from non-farstream element");

          ts_fail_unless (
              gst_structure_has_field_typed (s, "session", FS_TYPE_SESSION),
              "farstream-current-send-codec-changed structure"
              " has no session field");
          ts_fail_unless (
              gst_structure_has_field_typed (s, "codec",
                  FS_TYPE_CODEC),
              "");

          value = gst_structure_get_value (s, "session");
          session = g_value_get_object (value);
          value = gst_structure_get_value (s, "codec");
          codec = g_value_get_boxed (value);

          ts_fail_unless (session && codec,
              "current-send-codec-changed with NULL session(%p) or codec(%p)",
              session, codec);

          _current_send_codec_changed (session, codec);
        }
        else if (gst_structure_has_name (s,
                "farstream-local-candidates-prepared"))
        {
          FsStream *stream;
          const GValue *value;

          ts_fail_unless (
            FS_IS_CONFERENCE (GST_MESSAGE_SRC (message)),
            "Received farstream-error from non-farstream element");

          ts_fail_unless (
              gst_structure_has_field_typed (s, "stream", FS_TYPE_STREAM),
              "farstream-local-candidates-prepared structure"
              " has no stream field");

          value = gst_structure_get_value (s, "stream");
          stream = g_value_get_object (value);

          _local_candidates_prepared (stream);
        }

       }
      break;
    case GST_MESSAGE_ERROR:
      {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error (message, &error, &debug);

        ts_fail ("Got an error on the BUS (%d): %s (%s)", error->code,
            error->message, debug);
        g_error_free (error);
        g_free (debug);
      }
      break;
    case GST_MESSAGE_WARNING:
      {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_warning (message, &error, &debug);

        GST_WARNING ("%d: Got a warning on the BUS (%d): %s (%s)", dat->id,
            error->code,
            error->message, debug);
        g_error_free (error);
        g_free (debug);
      }
      break;
    default:
      break;
  }

  return TRUE;
}

static void
_handoff_handler (GstElement *element, GstBuffer *buffer, GstPad *pad,
  gpointer user_data)
{
  struct SimpleTestStream *st = user_data;
  int i;
  gboolean stop = TRUE;
  GList *codecs = NULL;

  TEST_LOCK ();

  if (st->dat->destroyed)
  {
    TEST_UNLOCK ();
    return;
  }

  g_object_get (st->dat->session,
      "codecs", &codecs,
      NULL);

  ts_fail_if (codecs == NULL, "Could not get codecs");

  if (st->flags & WAITING_ON_LAST_CODEC)
  {
    if (fs_codec_are_equal (
        g_list_last (codecs)->data,
        g_object_get_data (G_OBJECT (element), "codec")))
    {
      st->flags &= ~WAITING_ON_LAST_CODEC;
      st->flags |= SHOULD_BE_LAST_CODEC;
      max_buffer_count += st->buffer_count;
      GST_DEBUG ("We HAVE last codec");
    }
    else
    {
#if 0
      gchar *str = fs_codec_to_string (
          g_object_get_data (G_OBJECT (element), "codec"));
      gchar *str2 = fs_codec_to_string (g_list_last (codecs)->data);
      GST_DEBUG ("not yet the last codec, skipping (we have %s, we want %s)",
          str, str2);
      g_free (str);
      g_free (str2);
#endif
      fs_codec_list_destroy (codecs);
      TEST_UNLOCK ();
      return;
    }
  }


  if (select_last_codec || st->flags & SHOULD_BE_LAST_CODEC)
  {
    if (!fs_codec_are_equal (
            g_list_last (codecs)->data,
            g_object_get_data (G_OBJECT (element), "codec")))
    {
      if (!reset_to_last_codec)
        ts_fail ("The handoff handler got a buffer from the wrong codec"
            " (ie. not the last)");
      fs_codec_list_destroy (codecs);
      TEST_UNLOCK ();
      return;
    }
  }
  else
    ts_fail_unless (
        fs_codec_are_equal (
            g_list_first (codecs)->data,
            g_object_get_data (G_OBJECT (element), "codec")),
        "The handoff handler got a buffer from the wrong codec");

  fs_codec_list_destroy (codecs);


  st->buffer_count++;
  GST_LOG ("%d:%d: Buffer %d", st->dat->id, st->target->id, st->buffer_count);


  /*
  ts_fail_if (dat->buffer_count > max_buffer_count,
    "Too many buffers %d > max_buffer_count", dat->buffer_count);
  */

  for (i = 0; i < count && !stop ; i++)
  {
    GList *item;


    for (item = g_list_first (dats[i]->streams);
         item;
         item = g_list_next (item))
    {
      struct SimpleTestStream *st2 = item->data;

      if (st2->buffer_count < max_buffer_count)
      {
        stop = FALSE;
        break;
      }
    }
  }

  if (stop)
  {
    if (reset_to_last_codec && !(st->flags & HAS_BEEN_RESET)) {
      //GError *error = NULL;
      GList *nego_codecs = NULL;
      //gchar *str = NULL;

      g_object_get (st->target->session,
          "codecs", &nego_codecs,
          NULL);
#if 0
      ts_fail_if (nego_codecs == NULL, "No codecs");
      ts_fail_if (g_list_length (nego_codecs) < 2, "Only one negotiated codec");

      str = fs_codec_to_string (g_list_last (nego_codecs)->data);
      GST_DEBUG ("Setting codec to: %s", str);
      g_free (str);

      ts_fail_unless (fs_session_set_send_codec (st->target->session,
              g_list_last (nego_codecs)->data, &error),
          "Could not set the send codec: %s",
          error ? error->message : "NO GError!!!");
      g_clear_error (&error);
#endif
      fs_codec_list_destroy (nego_codecs);

      st->flags |= HAS_BEEN_RESET | WAITING_ON_LAST_CODEC;

      GST_DEBUG ("RESET TO LAST CODEC");

    } else {
      g_main_loop_quit (loop);
    }
  }

  TEST_UNLOCK ();
}

static void
_src_pad_added (FsStream *stream, GstPad *pad, FsCodec *codec,
    gpointer user_data)
{
  struct SimpleTestStream *st = user_data;
  GstElement *fakesink = gst_element_factory_make ("fakesink", NULL);
  GstPad *fakesink_pad = NULL;
  GstPadLinkReturn ret;
  FsCodec *codeccopy = fs_codec_copy (codec);
  gchar *str = NULL;

  g_assert (fakesink);

  g_object_set (fakesink,
      "signal-handoffs", TRUE,
      "sync", TRUE,
      "async", TRUE,
      NULL);

  ts_fail_if (codec->encoding_name == NULL,
      "Got invalid codec without an encoding_name with id %u"
      " and clock_rate %u", codec->id, codec->clock_rate);

  g_object_set_data (G_OBJECT (fakesink), "codec", codeccopy);
  g_object_weak_ref (G_OBJECT (fakesink),
      (GWeakNotify) fs_codec_destroy, codeccopy);

  g_signal_connect (fakesink, "handoff", st->handoff_handler, st);

  gst_bin_add (GST_BIN (st->dat->pipeline), fakesink);

  fakesink_pad = gst_element_get_static_pad (fakesink, "sink");
  ret = gst_pad_link (pad, fakesink_pad);
  gst_object_unref (fakesink_pad);

  ts_fail_if (GST_PAD_LINK_FAILED(ret), "Could not link fakesink");

  ts_fail_if (gst_element_set_state (fakesink, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE, "Could not set the fakesink to playing");

  str = fs_codec_to_string (codec);
  GST_DEBUG ("%d:%d: Added Fakesink for codec %s", st->dat->id, st->target->id,
           str);
  g_free (str);

  if (max_src_pads > 1)
    ts_fail_unless (count_stream_pads (stream) <= max_src_pads);
  else
    ts_fail_unless (count_stream_pads (stream) == 1);
}


static struct SimpleTestStream *
find_pointback_stream (
    struct SimpleTestConference *dat,
    struct SimpleTestConference *target)
{
  GList *item = NULL;

  for (item = g_list_first (dat->streams);
       item;
       item = g_list_next (item))
  {
    struct SimpleTestStream *st = item->data;

    if (st->target == target)
      return st;
  }

  ts_fail ("We did not find a return stream for %d in %d", target->id, dat->id);
  return NULL;
}


static void
rawconference_connect_signals (struct SimpleTestConference *dat)
{
  GstBus *bus = NULL;

  bus = gst_element_get_bus (dat->pipeline);
  gst_bus_add_watch (bus, _bus_callback, dat);
  gst_object_unref (bus);
}


static gboolean
_start_pipeline (gpointer user_data)
{
  struct SimpleTestConference *dat = user_data;

  GST_DEBUG ("%d: Starting pipeline", dat->id);

  ts_fail_if (gst_element_set_state (dat->pipeline, GST_STATE_PLAYING) ==
    GST_STATE_CHANGE_FAILURE, "Could not set the pipeline to playing");

  dat->started = TRUE;

  return FALSE;
}

static gboolean
_compare_codec_lists (GList *list1, GList *list2)
{
  for (; list1 && list2;
       list1 = g_list_next (list1),
       list2 = g_list_next (list2)) {
    if (!fs_codec_are_equal (list1->data, list2->data))
      return FALSE;
  }

  if (list1 == NULL && list2 == NULL)
    return TRUE;
  else
    return FALSE;
}

static void
_negotiated_codecs_notify (GObject *object, GParamSpec *paramspec,
    gpointer user_data)
{
  struct SimpleTestConference *dat = user_data;
  FsSession *session = FS_SESSION (object);
  GList *codecs = NULL;
  GError *error = NULL;
  GList *item = NULL;

  GST_DEBUG ("%d: New negotiated codecs", dat->id);

  ts_fail_if (session != dat->session, "Got signal from the wrong object");

  g_object_get (dat->session, "codecs", &codecs, NULL);
  ts_fail_if (codecs == NULL, "Could not get the negotiated codecs");


  /* We have to find the stream from the target that points back to us */
  for (item = g_list_first (dat->streams); item; item = g_list_next (item))
  {
    struct SimpleTestStream *st = item->data;
    struct SimpleTestStream *st2 = find_pointback_stream (st->target, dat);
    GList *rcodecs2;

    GST_DEBUG ("Setting negotiated remote codecs on %d:%d from %d",st2->dat->id,
        st2->target->id, dat->id);
    if (!fs_stream_set_remote_codecs (st2->stream, codecs, &error))
    {
      if (error)
        ts_fail ("Could not set the remote codecs on stream %d:%d (%d): %s",
            st2->dat->id, st2->target->id,
            error->code,
            error->message);
      else
        ts_fail ("Could not set the remote codecs on stream %d:%d"
            " and we DID not get a GError!!",
            st2->dat->id, st2->target->id);
    }
    g_object_get (st2->stream, "remote-codecs", &rcodecs2, NULL);
    ts_fail_unless (_compare_codec_lists (rcodecs2, codecs),
        "Can not get remote codecs correctly");

    fs_codec_list_destroy (rcodecs2);

    if (select_last_codec)
      ts_fail_unless (
          fs_session_set_send_codec (st2->dat->session,
              g_list_last (codecs)->data,
              &error),
          "Error setting the send codec to the last codec: %s",
          error ? error->message : "No GError");

    g_clear_error (&error);
    break;
  }
  fs_codec_list_destroy (codecs);
}


static void
set_initial_codecs (
    struct SimpleTestConference *from,
    struct SimpleTestStream *to)
{
  FsCodec *codec;
  GList *codecs = NULL;
  GList *filtered_codecs = NULL;
  GList *rcodecs2 = NULL;
  GError *error = NULL;

  if (to->destroyed || from->destroyed)
    return;

  g_object_get (from->session, "codecs", &codecs, NULL);

  ts_fail_unless (codecs == NULL, "Shouldn't generate codecs codecs");

  codec = fs_codec_new (0, "audio/x-raw,"
      "format=(string)"GST_AUDIO_NE (S16)","
      "rate=(int)44100, channels=(int)1", FS_MEDIA_TYPE_AUDIO, 0);
  codecs = g_list_append (codecs, codec);

  filtered_codecs = g_list_append (filtered_codecs, codecs->data);

  GST_DEBUG ("Setting initial remote codecs on %d:%d from %d",
      to->dat->id, to->target->id,
      from->id);

  if (!fs_stream_set_remote_codecs (to->stream, filtered_codecs, &error))
  {
    if (error)
      ts_fail ("Could not set the remote codecs on stream %d:%d (%d): %s",
          to->dat->id, to->target->id,
          error->code,
          error->message);
    else
      ts_fail ("Could not set the remote codecs on stream %d"
          " and we DID not get a GError!!", to->target->id);
  }
  g_object_get (to->stream, "remote-codecs", &rcodecs2, NULL);
  ts_fail_unless (_compare_codec_lists (rcodecs2, filtered_codecs),
      "Can not get remote codecs correctly");
  fs_codec_list_destroy (rcodecs2);


  if (select_last_codec)
    ts_fail_unless (
        fs_session_set_send_codec (to->dat->session,
            g_list_last (filtered_codecs)->data,
            &error),
        "Error setting the send codec to the last codec: %s",
        error ? error->message : "No GError");
  g_clear_error (&error);

  g_list_free (filtered_codecs);
  fs_codec_list_destroy (codecs);
}

typedef void (*extra_conf_init) (struct SimpleTestConference *dat,
    guint confid);
typedef void (*extra_stream_init) (struct SimpleTestStream *st, guint confid,
    guint streamid);

static void
nway_test (int in_count, extra_conf_init extra_conf_init,
    extra_stream_init extra_stream_init, const gchar *transmitter,
    guint st_param_count, GParameter *st_params)
{
  int i, j;
  GParameter *params = NULL;


  if (!strcmp ("rawudp", transmitter))
  {
    params = g_new0 (GParameter, st_param_count+2);

    memcpy (params, st_params, st_param_count * sizeof (GParameter));

    params[st_param_count].name = "upnp-discovery";
    g_value_init (&params[st_param_count].value, G_TYPE_BOOLEAN);
    g_value_set_boolean (&params[st_param_count].value, FALSE);

    params[st_param_count+1].name = "upnp-mapping";
    g_value_init (&params[st_param_count+1].value, G_TYPE_BOOLEAN);
    g_value_set_boolean (&params[st_param_count+1].value, FALSE);

    st_param_count += 2;
    st_params = params;
  }

  count = in_count;

  loop = g_main_loop_new (NULL, FALSE);

  dats = g_new0 (struct SimpleTestConference *, count);

  for (i = 0; i < count; i++)
  {
    gchar *tmp = g_strdup_printf ("tester%d@hostname", i);
    dats[i] = setup_simple_conference (i, "fsrawconference", tmp);
    g_free (tmp);


    if (extra_conf_init)
      extra_conf_init (dats[i], i);

    rawconference_connect_signals (dats[i]);
    g_idle_add (_start_pipeline, dats[i]);

    setup_fakesrc (dats[i]);

    if (i != 0)
      g_signal_connect (dats[i]->session, "notify::codecs",
          G_CALLBACK (_negotiated_codecs_notify), dats[i]);
  }

  TEST_LOCK ();

  for (i = 0; i < count; i++)
    for (j = 0; j < count; j++)
      if (i != j)
      {
        struct SimpleTestStream *st = NULL;

        st = simple_conference_add_stream (dats[i], dats[j], transmitter,
            st_param_count, st_params);
        st->handoff_handler = G_CALLBACK (_handoff_handler);
        g_signal_connect (st->stream, "src-pad-added",
            G_CALLBACK (_src_pad_added), st);

        if (!strcmp ("shm", transmitter))
        {
          FsCandidate *candidate = fs_candidate_new ("1", 1,
              FS_CANDIDATE_TYPE_HOST, FS_NETWORK_PROTOCOL_UDP,
              "/tmp/test-stream", 0);
          fs_stream_force_remote_candidates (st->stream,
              g_list_prepend (NULL, candidate), NULL);
        }

        if (extra_stream_init)
          extra_stream_init (st, i, j);
      }


  for (i = 1; i < count; i++)
  {
    struct SimpleTestStream *st = find_pointback_stream (dats[i], dats[0]);
    set_initial_codecs (dats[0], st);
  }

  TEST_UNLOCK ();

  g_main_loop_run (loop);

  for (i = 0; i < count; i++)
    gst_element_set_state (dats[i]->pipeline, GST_STATE_NULL);

  for (i = 0; i < count; i++)
    cleanup_simple_conference (dats[i]);

  g_free (dats);

  g_main_loop_unref (loop);

  g_free (params);
}


GST_START_TEST (test_rawconference_two_way)
{
  nway_test (2, NULL, NULL, "rawudp", 0, NULL);
}
GST_END_TEST;


GST_START_TEST (test_rawconference_two_way_shm)
{
  nway_test (2, NULL, NULL, "shm", 0, NULL);
}
GST_END_TEST;


GST_START_TEST (test_rawconference_errors)
{
  struct SimpleTestConference *dat = NULL;
  FsParticipant *participant = NULL;
  FsStream *stream = NULL;
  GError *error = NULL;

  dat = setup_simple_conference (1, "fsrawconference", "bob@127.0.0.1");

  participant = fs_conference_new_participant (FS_CONFERENCE (dat->conference),
      NULL);
  ts_fail_if (participant == NULL, "Could not create participant");



  stream = fs_session_new_stream (dat->session, participant, FS_DIRECTION_NONE,
      &error);
  ts_fail_unless (stream != NULL);

  fail_unless (fs_stream_set_transmitter (stream, "invalid-transmitter-name",
          NULL, 0, &error) == FALSE);
  ts_fail_if (error == NULL, "Error was not set");
  ts_fail_unless (error->domain == FS_ERROR &&
      error->code == FS_ERROR_CONSTRUCTION,
      "The wrong domain or code (%d) was returned", error->code);

  g_clear_error (&error);

  g_object_unref (participant);

  cleanup_simple_conference (dat);

}
GST_END_TEST;


static void
_error_handoff_handler (GstElement *element, GstBuffer *buffer, GstPad *pad,
  gpointer user_data)
{
  ts_fail ("Received a buffer when we shouldn't have");
}

static void
_normal_handoff_handler (GstElement *element, GstBuffer *buffer, GstPad *pad,
  gpointer user_data)
{
  struct SimpleTestStream *st = user_data;

  st->buffer_count++;

  if (st->buffer_count > 100)
    g_main_loop_quit (loop);

}

gint error_conf;

static void
_recv_only_init (struct SimpleTestStream *st, guint confid, guint streamid)
{
  if (confid == error_conf)
  {
    st->handoff_handler = G_CALLBACK (_error_handoff_handler);
  }
  else
  {
    st->handoff_handler = G_CALLBACK (_normal_handoff_handler);
    g_object_set (st->stream, "direction", FS_DIRECTION_RECV, NULL);
  }

}

GST_START_TEST (test_rawconference_recv_only)
{
  error_conf = 0;
  nway_test (2, NULL, _recv_only_init, "rawudp", 0, NULL);
  error_conf = 1;
  nway_test (2, NULL, _recv_only_init, "rawudp", 0, NULL);
}
GST_END_TEST;

static void
_send_only_init (struct SimpleTestStream *st, guint confid, guint streamid)
{
  if (confid == error_conf)
  {
    st->handoff_handler = G_CALLBACK (_error_handoff_handler);
    g_object_set (st->stream, "direction", FS_DIRECTION_SEND, NULL);
  }
  else
  {
    st->handoff_handler = G_CALLBACK (_normal_handoff_handler);
  }
}

GST_START_TEST (test_rawconference_send_only)
{
  error_conf = 0;
  nway_test (2, NULL, _send_only_init, "rawudp", 0, NULL);
  error_conf = 1;
  nway_test (2, NULL, _send_only_init, "rawudp", 0, NULL);
}
GST_END_TEST;



static void
_switch_handoff_handler (GstElement *element, GstBuffer *buffer, GstPad *pad,
  gpointer user_data)
{
  struct SimpleTestStream *st = user_data;

  st->buffer_count++;

  if (st->buffer_count == 20)
    g_object_set (st->stream, "direction", FS_DIRECTION_SEND, NULL);

  if (st->buffer_count > 20)
    ts_fail ("Received a buffer on a stream that should have been sendonly");
}


static void
_change_to_send_only_init (struct SimpleTestStream *st, guint confid,
    guint streamid)
{
  if (confid == 0)
    st->handoff_handler = G_CALLBACK (_normal_handoff_handler);
  else
    st->handoff_handler = G_CALLBACK (_switch_handoff_handler);
}

GST_START_TEST (test_rawconference_change_to_send_only)
{
  nway_test (2, NULL, _change_to_send_only_init, "rawudp", 0, NULL);
}
GST_END_TEST;


GST_START_TEST (test_rawconference_dispose)
{
  FsConference *conf;
  FsParticipant *part;
  FsSession *session;
  FsStream *stream;
  GError *error = NULL;

  conf = FS_CONFERENCE (gst_element_factory_make ("fsrawconference", NULL));
  fail_if (conf == NULL);

  session = fs_conference_new_session (conf, FS_MEDIA_TYPE_AUDIO, &error);
  fail_if (session == NULL || error != NULL);

  part = fs_conference_new_participant (conf, &error);
  fail_if (part == NULL || error != NULL);

  stream = fs_session_new_stream (session, part, FS_DIRECTION_BOTH, &error);
  fail_if (stream == NULL || error != NULL);

  fs_stream_destroy (stream);

  fail_if (fs_stream_add_remote_candidates (stream, NULL, &error));
  fail_unless (error->domain == FS_ERROR && error->code == FS_ERROR_DISPOSED);
  g_clear_error (&error);

  fail_if (fs_stream_set_remote_codecs (stream, NULL, &error));
  fail_unless (error->domain == FS_ERROR && error->code == FS_ERROR_DISPOSED);
  g_clear_error (&error);

  fail_if (fs_stream_force_remote_candidates (stream, NULL, &error));
  fail_unless (error->domain == FS_ERROR && error->code == FS_ERROR_DISPOSED);
  g_clear_error (&error);

  fs_stream_destroy (stream);
  g_object_unref (stream);

  stream = fs_session_new_stream (session, part, FS_DIRECTION_BOTH, &error);
  fail_if (stream == NULL || error != NULL);

  fs_stream_destroy (stream);

  fail_if (fs_stream_add_remote_candidates (stream, NULL, &error));
  fail_unless (error->domain == FS_ERROR && error->code == FS_ERROR_DISPOSED);
  g_clear_error (&error);

  fail_if (fs_stream_set_remote_codecs (stream, NULL, &error));
  fail_unless (error->domain == FS_ERROR && error->code == FS_ERROR_DISPOSED);
  g_clear_error (&error);

  fail_if (fs_stream_force_remote_candidates (stream, NULL, &error));
  fail_unless (error->domain == FS_ERROR && error->code == FS_ERROR_DISPOSED);
  g_clear_error (&error);

  fs_session_destroy (session);

  fail_if (fs_session_start_telephony_event (session, 1, 2));
  fail_if (fs_session_stop_telephony_event (session));

  fail_if (fs_session_set_send_codec (session, NULL, &error));
  fail_unless (error->domain == FS_ERROR &&
      error->code == FS_ERROR_NOT_IMPLEMENTED);
  g_clear_error (&error);

  fail_if (fs_session_set_codec_preferences (session, NULL, &error));
  fail_unless (error->domain == FS_ERROR &&
      error->code == FS_ERROR_NOT_IMPLEMENTED);
  g_clear_error (&error);

  fs_session_destroy (session);
  g_object_unref (session);
  g_object_unref (part);
  g_object_unref (stream);
  gst_object_unref (conf);
}
GST_END_TEST;


static Suite *
fsrawconference_suite (void)
{
  Suite *s = suite_create ("fsrawconference");
  TCase *tc_chain;

  tc_chain = tcase_create ("fsrawconference_base");
  tcase_add_test (tc_chain, test_rawconference_new);
  suite_add_tcase (s, tc_chain);

  tc_chain = tcase_create ("fsrawconference_two_way");
  tcase_add_test (tc_chain, test_rawconference_two_way);
  suite_add_tcase (s, tc_chain);

  tc_chain = tcase_create ("fsrawconference_two_way_shm");
  tcase_add_test (tc_chain, test_rawconference_two_way_shm);
  //suite_add_tcase (s, tc_chain);

  tc_chain = tcase_create ("fsrawconference_errors");
  tcase_add_test (tc_chain, test_rawconference_errors);
  suite_add_tcase (s, tc_chain);

  tc_chain = tcase_create ("fsrawconference_recv_only");
  tcase_add_test (tc_chain, test_rawconference_recv_only);
  suite_add_tcase (s, tc_chain);

  tc_chain = tcase_create ("fsrawconference_send_only");
  tcase_add_test (tc_chain, test_rawconference_send_only);
  suite_add_tcase (s, tc_chain);

  tc_chain = tcase_create ("fsrawconference_change_to_send_only");
  tcase_add_test (tc_chain, test_rawconference_change_to_send_only);
  suite_add_tcase (s, tc_chain);

  tc_chain = tcase_create ("fsrawconference_dispose");
  tcase_add_test (tc_chain, test_rawconference_dispose);
  suite_add_tcase (s, tc_chain);

  return s;
}

GST_CHECK_MAIN (fsrawconference);
