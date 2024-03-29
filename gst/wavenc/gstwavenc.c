/* -*- mOde: C; tab-width: 2; indent-tabs-mode: t; c-basic-offset: 2 -*- */
/* GStreamer .wav encoder
 * Copyright (C) <2002> Iain Holmes <iain@prettypeople.org>
 * Copyright (C) <2006> Tim-Philipp Müller <tim centricular net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 * 
 */
/**
 * SECTION:element-wavenc
 *
 * Format an audio stream into the wav format.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 cdparanoiasrc mode=continuous ! queue ! audioconvert ! wavenc ! filesink location=cd.wav
 * ]| Rip a whole audio CD into a single wav file, with the track table written into a CUE sheet inside the file
 * |[
 * gst-launch-1.0 cdparanoiasrc track=5 ! queue ! audioconvert ! wavenc ! filesink location=track5.wav
 * ]| Rip track 5 of an audio CD into a single wav file containing unencoded raw audio samples.
 * </refsect2>
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "gstwavenc.h"

#include <gst/audio/audio.h>
#include <gst/riff/riff-media.h>
#include <gst/base/gstbytewriter.h>

GST_DEBUG_CATEGORY_STATIC (wavenc_debug);
#define GST_CAT_DEFAULT wavenc_debug

struct riff_struct
{
  guint8 id[4];                 /* RIFF */
  guint32 len;
  guint8 wav_id[4];             /* WAVE */
};

struct chunk_struct
{
  guint8 id[4];
  guint32 len;
};

struct common_struct
{
  guint16 wFormatTag;
  guint16 wChannels;
  guint32 dwSamplesPerSec;
  guint32 dwAvgBytesPerSec;
  guint16 wBlockAlign;
  guint16 wBitsPerSample;       /* Only for PCM */
};

struct wave_header
{
  struct riff_struct riff;
  struct chunk_struct format;
  struct common_struct common;
  struct chunk_struct data;
};

typedef struct
{
  /* Offset Size    Description   Value
   * 0x00   4       ID            unique identification value
   * 0x04   4       Position      play order position
   * 0x08   4       Data Chunk ID RIFF ID of corresponding data chunk
   * 0x0c   4       Chunk Start   Byte Offset of Data Chunk *
   * 0x10   4       Block Start   Byte Offset to sample of First Channel
   * 0x14   4       Sample Offset Byte Offset to sample byte of First Channel
   */
  guint32 id;
  guint32 position;
  guint8 data_chunk_id[4];
  guint32 chunk_start;
  guint32 block_start;
  guint32 sample_offset;
} GstWavEncCue;

typedef struct
{
  /* Offset Size    Description     Value
   * 0x00   4       Chunk ID        "labl" (0x6C61626C) or "note" (0x6E6F7465)
   * 0x04   4       Chunk Data Size depends on contained text
   * 0x08   4       Cue Point ID    0 - 0xFFFFFFFF
   * 0x0c           Text
   */
  guint8 chunk_id[4];
  guint32 chunk_data_size;
  guint32 cue_point_id;
  gchar *text;
} GstWavEncLabl, GstWavEncNote;

/* FIXME: mono doesn't produce correct files it seems, at least mplayer xruns */
/* Max. of two channels, more channels need WAVFORMATEX with
 * channel layout, which we do not support yet */
#define SINK_CAPS \
    "audio/x-raw, "                      \
    "rate = (int) [ 1, MAX ], "          \
    "channels = (int) 1, "               \
    "format = (string) { S32LE, S24LE, S16LE, U8, F32LE, F64LE }, " \
    "layout = (string) interleaved"      \
    "; "                                 \
    "audio/x-raw, "                      \
    "rate = (int) [ 1, MAX ], "          \
    "channels = (int) 2, "               \
    "channel-mask = (bitmask) 0x3, "     \
    "format = (string) { S32LE, S24LE, S16LE, U8, F32LE, F64LE }, " \
    "layout = (string) interleaved"      \
    "; "                                 \
    "audio/x-alaw, "                     \
    "rate = (int) [ 8000, 192000 ], "    \
    "channels = (int) [ 1, 2 ]; "        \
    "audio/x-mulaw, "                    \
    "rate = (int) [ 8000, 192000 ], "    \
    "channels = (int) [ 1, 2 ]"


static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (SINK_CAPS)
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-wav")
    );

#define gst_wavenc_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstWavEnc, gst_wavenc, GST_TYPE_ELEMENT,
    G_IMPLEMENT_INTERFACE (GST_TYPE_TAG_SETTER, NULL)
    G_IMPLEMENT_INTERFACE (GST_TYPE_TOC_SETTER, NULL)
    );

static GstFlowReturn gst_wavenc_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf);
static gboolean gst_wavenc_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static GstStateChangeReturn gst_wavenc_change_state (GstElement * element,
    GstStateChange transition);
static gboolean gst_wavenc_sink_setcaps (GstPad * pad, GstCaps * caps);

static void
gst_wavenc_class_init (GstWavEncClass * klass)
{
  GstElementClass *element_class;

  element_class = (GstElementClass *) klass;

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_wavenc_change_state);

  gst_element_class_set_static_metadata (element_class, "WAV audio muxer",
      "Codec/Muxer/Audio",
      "Encode raw audio into WAV", "Iain Holmes <iain@prettypeople.org>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));

  GST_DEBUG_CATEGORY_INIT (wavenc_debug, "wavenc", 0, "WAV encoder element");
}

static void
gst_wavenc_init (GstWavEnc * wavenc)
{
  wavenc->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_chain_function (wavenc->sinkpad,
      GST_DEBUG_FUNCPTR (gst_wavenc_chain));
  gst_pad_set_event_function (wavenc->sinkpad,
      GST_DEBUG_FUNCPTR (gst_wavenc_event));
  gst_pad_use_fixed_caps (wavenc->sinkpad);
  gst_element_add_pad (GST_ELEMENT (wavenc), wavenc->sinkpad);

  wavenc->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_pad_use_fixed_caps (wavenc->srcpad);
  gst_pad_set_caps (wavenc->srcpad,
      gst_static_pad_template_get_caps (&src_factory));
  gst_element_add_pad (GST_ELEMENT (wavenc), wavenc->srcpad);
}

#define WAV_HEADER_LEN 44

static GstBuffer *
gst_wavenc_create_header_buf (GstWavEnc * wavenc)
{
  struct wave_header wave;
  GstBuffer *buf;
  GstMapInfo map;
  guint8 *header;

  buf = gst_buffer_new_and_alloc (WAV_HEADER_LEN);
  gst_buffer_map (buf, &map, GST_MAP_WRITE);
  header = map.data;
  memset (header, 0, WAV_HEADER_LEN);

  memcpy (wave.riff.id, "RIFF", 4);
  wave.riff.len =
      wavenc->meta_length + wavenc->audio_length + WAV_HEADER_LEN - 8;
  memcpy (wave.riff.wav_id, "WAVE", 4);

  memcpy (wave.format.id, "fmt ", 4);
  wave.format.len = 16;

  wave.common.wChannels = wavenc->channels;
  wave.common.wBitsPerSample = wavenc->width;
  wave.common.dwSamplesPerSec = wavenc->rate;
  wave.common.wFormatTag = wavenc->format;
  wave.common.wBlockAlign = (wavenc->width / 8) * wave.common.wChannels;
  wave.common.dwAvgBytesPerSec =
      wave.common.wBlockAlign * wave.common.dwSamplesPerSec;

  memcpy (wave.data.id, "data", 4);
  wave.data.len = wavenc->audio_length;

  memcpy (header, (char *) wave.riff.id, 4);
  GST_WRITE_UINT32_LE (header + 4, wave.riff.len);
  memcpy (header + 8, (char *) wave.riff.wav_id, 4);
  memcpy (header + 12, (char *) wave.format.id, 4);
  GST_WRITE_UINT32_LE (header + 16, wave.format.len);
  GST_WRITE_UINT16_LE (header + 20, wave.common.wFormatTag);
  GST_WRITE_UINT16_LE (header + 22, wave.common.wChannels);
  GST_WRITE_UINT32_LE (header + 24, wave.common.dwSamplesPerSec);
  GST_WRITE_UINT32_LE (header + 28, wave.common.dwAvgBytesPerSec);
  GST_WRITE_UINT16_LE (header + 32, wave.common.wBlockAlign);
  GST_WRITE_UINT16_LE (header + 34, wave.common.wBitsPerSample);
  memcpy (header + 36, (char *) wave.data.id, 4);
  GST_WRITE_UINT32_LE (header + 40, wave.data.len);

  gst_buffer_unmap (buf, &map);

  return buf;
}

static GstFlowReturn
gst_wavenc_push_header (GstWavEnc * wavenc)
{
  GstFlowReturn ret;
  GstBuffer *outbuf;
  GstSegment segment;

  /* seek to beginning of file */
  gst_segment_init (&segment, GST_FORMAT_BYTES);
  gst_pad_push_event (wavenc->srcpad, gst_event_new_segment (&segment));

  GST_DEBUG_OBJECT (wavenc, "writing header, meta_size=%u, audio_size=%u",
      wavenc->meta_length, wavenc->audio_length);

  outbuf = gst_wavenc_create_header_buf (wavenc);
  GST_BUFFER_OFFSET (outbuf) = 0;

  ret = gst_pad_push (wavenc->srcpad, outbuf);

  if (ret != GST_FLOW_OK) {
    GST_WARNING_OBJECT (wavenc, "push header failed: flow = %s",
        gst_flow_get_name (ret));
  }

  return ret;
}

static gboolean
gst_wavenc_sink_setcaps (GstPad * pad, GstCaps * caps)
{
  GstWavEnc *wavenc;
  GstStructure *structure;
  const gchar *name;
  gint chans, rate;
  GstCaps *ccaps;

  wavenc = GST_WAVENC (gst_pad_get_parent (pad));

  ccaps = gst_pad_get_current_caps (pad);
  if (wavenc->sent_header && ccaps && !gst_caps_can_intersect (caps, ccaps)) {
    gst_caps_unref (ccaps);
    GST_WARNING_OBJECT (wavenc, "cannot change format in middle of stream");
    goto fail;
  }
  if (ccaps)
    gst_caps_unref (ccaps);

  GST_DEBUG_OBJECT (wavenc, "got caps: %" GST_PTR_FORMAT, caps);

  structure = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (structure);

  if (!gst_structure_get_int (structure, "channels", &chans) ||
      !gst_structure_get_int (structure, "rate", &rate)) {
    GST_WARNING_OBJECT (wavenc, "caps incomplete");
    goto fail;
  }

  if (strcmp (name, "audio/x-raw") == 0) {
    GstAudioInfo info;

    if (!gst_audio_info_from_caps (&info, caps))
      goto fail;

    if (GST_AUDIO_INFO_IS_INTEGER (&info))
      wavenc->format = GST_RIFF_WAVE_FORMAT_PCM;
    else if (GST_AUDIO_INFO_IS_FLOAT (&info))
      wavenc->format = GST_RIFF_WAVE_FORMAT_IEEE_FLOAT;
    else
      goto fail;

    wavenc->width = GST_AUDIO_INFO_WIDTH (&info);
  } else if (strcmp (name, "audio/x-alaw") == 0) {
    wavenc->format = GST_RIFF_WAVE_FORMAT_ALAW;
    wavenc->width = 8;
  } else if (strcmp (name, "audio/x-mulaw") == 0) {
    wavenc->format = GST_RIFF_WAVE_FORMAT_MULAW;
    wavenc->width = 8;
  } else {
    GST_WARNING_OBJECT (wavenc, "Unsupported format %s", name);
    goto fail;
  }

  wavenc->channels = chans;
  wavenc->rate = rate;

  GST_LOG_OBJECT (wavenc,
      "accepted caps: format=0x%04x chans=%u width=%u rate=%u",
      wavenc->format, wavenc->channels, wavenc->width, wavenc->rate);

  gst_object_unref (wavenc);
  return TRUE;

fail:
  gst_object_unref (wavenc);
  return FALSE;
}

static void
gst_wavparse_tags_foreach (const GstTagList * tags, const gchar * tag,
    gpointer data)
{
  const struct
  {
    guint32 fcc;
    const gchar *tag;
  } rifftags[] = {
    {
    GST_RIFF_INFO_IARL, GST_TAG_LOCATION}, {
    GST_RIFF_INFO_IART, GST_TAG_ARTIST}, {
    GST_RIFF_INFO_ICMT, GST_TAG_COMMENT}, {
    GST_RIFF_INFO_ICOP, GST_TAG_COPYRIGHT}, {
    GST_RIFF_INFO_ICRD, GST_TAG_DATE}, {
    GST_RIFF_INFO_IGNR, GST_TAG_GENRE}, {
    GST_RIFF_INFO_IKEY, GST_TAG_KEYWORDS}, {
    GST_RIFF_INFO_INAM, GST_TAG_TITLE}, {
    GST_RIFF_INFO_IPRD, GST_TAG_ALBUM}, {
    GST_RIFF_INFO_ISBJ, GST_TAG_ALBUM_ARTIST}, {
    GST_RIFF_INFO_ISFT, GST_TAG_ENCODER}, {
    GST_RIFF_INFO_ISRC, GST_TAG_ISRC}, {
    0, NULL}
  };
  gint n;
  gchar *str = NULL;
  GstByteWriter *bw = data;
  for (n = 0; rifftags[n].fcc != 0; n++) {
    if (!strcmp (rifftags[n].tag, tag)) {
      if (rifftags[n].fcc == GST_RIFF_INFO_ICRD) {
        GDate *date;
        /* special case for the date tag */
        if (gst_tag_list_get_date (tags, tag, &date)) {
          str =
              g_strdup_printf ("%04d:%02d:%02d", g_date_get_year (date),
              g_date_get_month (date), g_date_get_day (date));
          g_date_free (date);
        }
      } else {
        gst_tag_list_get_string (tags, tag, &str);
      }
      if (str) {
        gst_byte_writer_put_uint32_le (bw, rifftags[n].fcc);
        gst_byte_writer_put_uint32_le (bw, GST_ROUND_UP_2 (strlen (str)));
        gst_byte_writer_put_string (bw, str);
        g_free (str);
        str = NULL;
        break;
      }
    }
  }

}

static GstFlowReturn
gst_wavenc_write_tags (GstWavEnc * wavenc)
{
  const GstTagList *user_tags;
  GstTagList *tags;
  guint size;
  GstBuffer *buf;
  GstByteWriter bw;

  g_return_val_if_fail (wavenc != NULL, GST_FLOW_OK);

  user_tags = gst_tag_setter_get_tag_list (GST_TAG_SETTER (wavenc));
  if ((!wavenc->tags) && (!user_tags)) {
    GST_DEBUG_OBJECT (wavenc, "have no tags");
    return GST_FLOW_OK;
  }
  tags =
      gst_tag_list_merge (user_tags, wavenc->tags,
      gst_tag_setter_get_tag_merge_mode (GST_TAG_SETTER (wavenc)));

  GST_DEBUG_OBJECT (wavenc, "writing tags");

  gst_byte_writer_init_with_size (&bw, 1024, FALSE);

  /* add LIST INFO chunk */
  gst_byte_writer_put_data (&bw, (const guint8 *) "LIST", 4);
  gst_byte_writer_put_uint32_le (&bw, 0);
  gst_byte_writer_put_data (&bw, (const guint8 *) "INFO", 4);

  /* add tags */
  gst_tag_list_foreach (tags, gst_wavparse_tags_foreach, &bw);

  /* sets real size of LIST INFO chunk */
  size = gst_byte_writer_get_pos (&bw);
  gst_byte_writer_set_pos (&bw, 4);
  gst_byte_writer_put_uint32_le (&bw, size - 8);

  gst_tag_list_unref (tags);

  buf = gst_byte_writer_reset_and_get_buffer (&bw);
  wavenc->meta_length += gst_buffer_get_size (buf);
  return gst_pad_push (wavenc->srcpad, buf);
}

static gboolean
gst_wavenc_is_cue_id_unique (guint32 id, GList * list)
{
  GstWavEncCue *cue;

  while (list) {
    cue = list->data;
    if (cue->id == id)
      return FALSE;
    list = g_list_next (list);
  }

  return TRUE;
}

static gboolean
gst_wavenc_parse_cue (GstWavEnc * wavenc, guint32 id, GstTocEntry * entry)
{
  gint64 start;
  GstWavEncCue *cue;

  g_return_val_if_fail (entry != NULL, FALSE);

  gst_toc_entry_get_start_stop_times (entry, &start, NULL);

  cue = g_new (GstWavEncCue, 1);
  cue->id = id;
  cue->position = gst_util_uint64_scale_round (start, wavenc->rate, GST_SECOND);
  memcpy (cue->data_chunk_id, "data", 4);
  cue->chunk_start = 0;
  cue->block_start = 0;
  cue->sample_offset = cue->position;
  wavenc->cues = g_list_append (wavenc->cues, cue);

  return TRUE;
}

static gboolean
gst_wavenc_parse_labl (GstWavEnc * wavenc, guint32 id, GstTocEntry * entry)
{
  gchar *tag;
  GstTagList *tags;
  GstWavEncLabl *labl;

  g_return_val_if_fail (entry != NULL, FALSE);

  tags = gst_toc_entry_get_tags (entry);
  if (!tags) {
    GST_INFO_OBJECT (wavenc, "no tags for entry: %d", id);
    return FALSE;
  }
  if (!gst_tag_list_get_string (tags, GST_TAG_TITLE, &tag)) {
    GST_INFO_OBJECT (wavenc, "no title tag for entry: %d", id);
    return FALSE;
  }

  labl = g_new (GstWavEncLabl, 1);
  memcpy (labl->chunk_id, "labl", 4);
  labl->chunk_data_size = 4 + strlen (tag) + 1;
  labl->cue_point_id = id;
  labl->text = tag;

  GST_DEBUG_OBJECT (wavenc, "got labl: '%s'", tag);

  wavenc->labls = g_list_append (wavenc->labls, labl);

  return TRUE;
}

static gboolean
gst_wavenc_parse_note (GstWavEnc * wavenc, guint32 id, GstTocEntry * entry)
{
  gchar *tag;
  GstTagList *tags;
  GstWavEncNote *note;

  g_return_val_if_fail (entry != NULL, FALSE);
  tags = gst_toc_entry_get_tags (entry);
  if (!tags) {
    GST_INFO_OBJECT (wavenc, "no tags for entry: %d", id);
    return FALSE;
  }
  if (!gst_tag_list_get_string (tags, GST_TAG_COMMENT, &tag)) {
    GST_INFO_OBJECT (wavenc, "no comment tag for entry: %d", id);
    return FALSE;
  }

  note = g_new (GstWavEncNote, 1);
  memcpy (note->chunk_id, "note", 4);
  note->chunk_data_size = 4 + strlen (tag) + 1;
  note->cue_point_id = id;
  note->text = tag;

  GST_DEBUG_OBJECT (wavenc, "got note: '%s'", tag);

  wavenc->notes = g_list_append (wavenc->notes, note);

  return TRUE;
}

static gboolean
gst_wavenc_write_cues (guint8 ** data, GList * list)
{
  GstWavEncCue *cue;

  while (list) {
    cue = list->data;
    GST_WRITE_UINT32_LE (*data, cue->id);
    GST_WRITE_UINT32_LE (*data + 4, cue->position);
    memcpy (*data + 8, (gchar *) cue->data_chunk_id, 4);
    GST_WRITE_UINT32_LE (*data + 12, cue->chunk_start);
    GST_WRITE_UINT32_LE (*data + 16, cue->block_start);
    GST_WRITE_UINT32_LE (*data + 20, cue->sample_offset);
    *data += 24;
    list = g_list_next (list);
  }

  return TRUE;
}

static gboolean
gst_wavenc_write_labls (guint8 ** data, GList * list)
{
  GstWavEncLabl *labl;

  while (list) {
    labl = list->data;
    memcpy (*data, (gchar *) labl->chunk_id, 4);
    GST_WRITE_UINT32_LE (*data + 4, labl->chunk_data_size);
    GST_WRITE_UINT32_LE (*data + 8, labl->cue_point_id);
    memcpy (*data + 12, (gchar *) labl->text, strlen (labl->text));
    *data += 8 + GST_ROUND_UP_2 (labl->chunk_data_size);
    list = g_list_next (list);
  }

  return TRUE;
}

static gboolean
gst_wavenc_write_notes (guint8 ** data, GList * list)
{
  GstWavEncNote *note;

  while (list) {
    note = list->data;
    memcpy (*data, (gchar *) note->chunk_id, 4);
    GST_WRITE_UINT32_LE (*data + 4, note->chunk_data_size);
    GST_WRITE_UINT32_LE (*data + 8, note->cue_point_id);
    memcpy (*data + 12, (gchar *) note->text, strlen (note->text));
    *data += 8 + GST_ROUND_UP_2 (note->chunk_data_size);
    list = g_list_next (list);
  }

  return TRUE;
}

static GstFlowReturn
gst_wavenc_write_toc (GstWavEnc * wavenc)
{
  GList *list;
  GstToc *toc;
  GstTocEntry *entry, *subentry;
  GstBuffer *buf;
  GstMapInfo map;
  guint8 *data;
  guint32 ncues, size, cues_size, labls_size, notes_size;

  if (!wavenc->toc) {
    GST_DEBUG_OBJECT (wavenc, "have no toc, checking toc_setter");
    wavenc->toc = gst_toc_setter_get_toc (GST_TOC_SETTER (wavenc));
  }
  if (!wavenc->toc) {
    GST_WARNING_OBJECT (wavenc, "have no toc");
    return GST_FLOW_OK;
  }

  toc = gst_toc_ref (wavenc->toc);
  size = 0;
  cues_size = 0;
  labls_size = 0;
  notes_size = 0;

  /* check if the TOC entries is valid */
  list = gst_toc_get_entries (toc);
  entry = list->data;
  if (gst_toc_entry_is_alternative (entry)) {
    list = gst_toc_entry_get_sub_entries (entry);
    while (list) {
      subentry = list->data;
      if (!gst_toc_entry_is_sequence (subentry))
        return FALSE;
      list = g_list_next (list);
    }
    list = gst_toc_entry_get_sub_entries (entry);
  }
  if (gst_toc_entry_is_sequence (entry)) {
    while (list) {
      entry = list->data;
      if (!gst_toc_entry_is_sequence (entry))
        return FALSE;
      list = g_list_next (list);
    }
    list = gst_toc_get_entries (toc);
  }

  ncues = g_list_length (list);
  GST_DEBUG_OBJECT (wavenc, "number of cue entries: %d", ncues);

  while (list) {
    guint32 id = 0;
    gint64 id64;
    const gchar *uid;

    entry = list->data;
    uid = gst_toc_entry_get_uid (entry);
    id64 = g_ascii_strtoll (uid, NULL, 0);
    /* check if id unique compatible with guint32 else generate random */
    if (id64 >= 0 && gst_wavenc_is_cue_id_unique (id64, wavenc->cues)) {
      id = (guint32) id64;
    } else {
      do {
        id = g_random_int ();
      } while (!gst_wavenc_is_cue_id_unique (id, wavenc->cues));
    }
    gst_wavenc_parse_cue (wavenc, id, entry);
    gst_wavenc_parse_labl (wavenc, id, entry);
    gst_wavenc_parse_note (wavenc, id, entry);
    list = g_list_next (list);
  }

  /* count cues size */
  if (wavenc->cues) {
    cues_size = 24 * g_list_length (wavenc->cues);
    size += 12 + cues_size;
  } else {
    GST_WARNING_OBJECT (wavenc, "cue's not found");
    return FALSE;
  }
  /* count labls size */
  if (wavenc->labls) {
    list = wavenc->labls;
    while (list) {
      GstWavEncLabl *labl;
      labl = list->data;
      labls_size += 8 + GST_ROUND_UP_2 (labl->chunk_data_size);
      list = g_list_next (list);
    }
    size += labls_size;
  }
  /* count notes size */
  if (wavenc->notes) {
    list = wavenc->notes;
    while (list) {
      GstWavEncNote *note;
      note = list->data;
      notes_size += 8 + GST_ROUND_UP_2 (note->chunk_data_size);
      list = g_list_next (list);
    }
    size += notes_size;
  }
  if (wavenc->labls || wavenc->notes) {
    size += 12;
  }

  buf = gst_buffer_new_and_alloc (size);
  gst_buffer_map (buf, &map, GST_MAP_WRITE);
  data = map.data;
  memset (data, 0, size);

  /* write Cue Chunk */
  if (wavenc->cues) {
    memcpy (data, (gchar *) "cue ", 4);
    GST_WRITE_UINT32_LE (data + 4, 4 + cues_size);
    GST_WRITE_UINT32_LE (data + 8, ncues);
    data += 12;
    gst_wavenc_write_cues (&data, wavenc->cues);

    /* write Associated Data List Chunk */
    if (wavenc->labls || wavenc->notes) {
      memcpy (data, (gchar *) "LIST", 4);
      GST_WRITE_UINT32_LE (data + 4, 4 + labls_size + notes_size);
      memcpy (data + 8, (gchar *) "adtl", 4);
      data += 12;
      if (wavenc->labls)
        gst_wavenc_write_labls (&data, wavenc->labls);
      if (wavenc->notes)
        gst_wavenc_write_notes (&data, wavenc->notes);
    }
  }

  /* free resources */
  if (toc)
    gst_toc_unref (toc);
  if (wavenc->cues)
    g_list_free_full (wavenc->cues, g_free);
  if (wavenc->labls)
    g_list_free_full (wavenc->labls, g_free);
  if (wavenc->notes)
    g_list_free_full (wavenc->notes, g_free);

  gst_buffer_unmap (buf, &map);
  wavenc->meta_length += gst_buffer_get_size (buf);

  return gst_pad_push (wavenc->srcpad, buf);
}

static gboolean
gst_wavenc_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean res = TRUE;
  GstWavEnc *wavenc;
  GstTagList *tags;
  GstToc *toc;

  wavenc = GST_WAVENC (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      gst_wavenc_sink_setcaps (pad, caps);

      /* have our own src caps */
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_EOS:
    {
      GstFlowReturn flow;
      GST_DEBUG_OBJECT (wavenc, "got EOS");

      flow = gst_wavenc_write_toc (wavenc);
      if (flow != GST_FLOW_OK) {
        GST_WARNING_OBJECT (wavenc, "error pushing toc: %s",
            gst_flow_get_name (flow));
      }
      flow = gst_wavenc_write_tags (wavenc);
      if (flow != GST_FLOW_OK) {
        GST_WARNING_OBJECT (wavenc, "error pushing tags: %s",
            gst_flow_get_name (flow));
      }

      /* write header with correct length values */
      gst_wavenc_push_header (wavenc);

      /* we're done with this file */
      wavenc->finished_properly = TRUE;

      /* and forward the EOS event */
      res = gst_pad_event_default (pad, parent, event);
      break;
    }
    case GST_EVENT_SEGMENT:
      /* Just drop it, it's probably in TIME format
       * anyway. We'll send our own newsegment event */
      gst_event_unref (event);
      break;
    case GST_EVENT_TOC:
      gst_event_parse_toc (event, &toc, NULL);
      if (toc) {
        if (wavenc->toc != toc) {
          if (wavenc->toc)
            gst_toc_unref (wavenc->toc);
          wavenc->toc = toc;
        } else {
          gst_toc_unref (toc);
        }
      }
      res = gst_pad_event_default (pad, parent, event);
      break;
    case GST_EVENT_TAG:
      gst_event_parse_tag (event, &tags);
      if (tags) {
        if (wavenc->tags != tags) {
          if (wavenc->tags)
            gst_tag_list_unref (wavenc->tags);
          wavenc->tags = gst_tag_list_ref (tags);
        }
      }
      res = gst_pad_event_default (pad, parent, event);
      break;
    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }

  return res;
}

static GstFlowReturn
gst_wavenc_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstWavEnc *wavenc = GST_WAVENC (parent);
  GstFlowReturn flow = GST_FLOW_OK;

  if (wavenc->channels <= 0) {
    GST_ERROR_OBJECT (wavenc, "Got data without caps");
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (G_UNLIKELY (!wavenc->sent_header)) {
    /* starting a file, means we have to finish it properly */
    wavenc->finished_properly = FALSE;

    /* push initial bogus header, it will be updated on EOS */
    flow = gst_wavenc_push_header (wavenc);
    if (flow != GST_FLOW_OK) {
      GST_WARNING_OBJECT (wavenc, "error pushing header: %s",
          gst_flow_get_name (flow));
      return flow;
    }
    GST_DEBUG_OBJECT (wavenc, "wrote dummy header");
    wavenc->audio_length = 0;
    wavenc->sent_header = TRUE;
  }

  GST_LOG_OBJECT (wavenc,
      "pushing %" G_GSIZE_FORMAT " bytes raw audio, ts=%" GST_TIME_FORMAT,
      gst_buffer_get_size (buf), GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)));

  buf = gst_buffer_make_writable (buf);

  GST_BUFFER_OFFSET (buf) = WAV_HEADER_LEN + wavenc->audio_length;
  GST_BUFFER_OFFSET_END (buf) = GST_BUFFER_OFFSET_NONE;

  wavenc->audio_length += gst_buffer_get_size (buf);

  flow = gst_pad_push (wavenc->srcpad, buf);

  return flow;
}

static GstStateChangeReturn
gst_wavenc_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstWavEnc *wavenc = GST_WAVENC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      wavenc->format = 0;
      wavenc->channels = 0;
      wavenc->width = 0;
      wavenc->rate = 0;
      /* use bogus size initially, we'll write the real
       * header when we get EOS and know the exact length */
      wavenc->audio_length = 0x7FFF0000;
      wavenc->meta_length = 0;
      wavenc->sent_header = FALSE;
      /* its true because we haven't writen anything */
      wavenc->finished_properly = TRUE;
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret != GST_STATE_CHANGE_SUCCESS)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (!wavenc->finished_properly) {
        GST_ELEMENT_WARNING (wavenc, STREAM, MUX,
            ("Wav stream not finished properly"),
            ("Wav stream not finished properly, no EOS received "
                "before shutdown"));
      }
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      GST_DEBUG_OBJECT (wavenc, "tags: %p", wavenc->tags);
      if (wavenc->tags) {
        gst_tag_list_unref (wavenc->tags);
        wavenc->tags = NULL;
      }
      GST_DEBUG_OBJECT (wavenc, "toc: %p", wavenc->toc);
      if (wavenc->toc) {
        gst_toc_unref (wavenc->toc);
        wavenc->toc = NULL;
      }
      gst_tag_setter_reset_tags (GST_TAG_SETTER (wavenc));
      gst_toc_setter_reset (GST_TOC_SETTER (wavenc));
      break;
    default:
      break;
  }

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "wavenc", GST_RANK_PRIMARY,
      GST_TYPE_WAVENC);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    wavenc,
    "Encode raw audio into WAV",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
