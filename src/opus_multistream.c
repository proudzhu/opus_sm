/* Copyright (c) 2011 Xiph.Org Foundation
   Written by Jean-Marc Valin */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "opus_multistream.h"
#include "opus.h"
#include "opus_private.h"
#include "stack_alloc.h"
#include <stdarg.h>
#include "float_cast.h"
#include "os_support.h"

typedef struct ChannelLayout {
   int nb_channels;
   int nb_streams;
   int nb_coupled_streams;
   unsigned char mapping[256];
} ChannelLayout;

struct OpusMSEncoder {
   ChannelLayout layout;
   int bitrate;
   /* Encoder states go here */
};

struct OpusMSDecoder {
   ChannelLayout layout;
   /* Decoder states go here */
};


#ifdef FIXED_POINT
#define opus_encode_native opus_encode
#else
#define opus_encode_native opus_encode_float
#endif

static int validate_layout(const ChannelLayout *layout)
{
   int i, max_channel;

   max_channel = layout->nb_streams+layout->nb_coupled_streams;
   if (max_channel>255)
      return 0;
   for (i=0;i<layout->nb_channels;i++)
   {
      if (layout->mapping[i] >= max_channel && layout->mapping[i] != 255)
         return 0;
   }
   return 1;
}


static int get_left_channel(const ChannelLayout *layout, int stream_id, int prev)
{
   int i;
   i = (prev<0) ? 0 : prev+1;
   for (;i<layout->nb_channels;i++)
   {
      if (layout->mapping[i]==stream_id*2)
         return i;
   }
   return -1;
}

static int get_right_channel(const ChannelLayout *layout, int stream_id, int prev)
{
   int i;
   i = (prev<0) ? 0 : prev+1;
   for (;i<layout->nb_channels;i++)
   {
      if (layout->mapping[i]==stream_id*2+1)
         return i;
   }
   return -1;
}

static int get_mono_channel(const ChannelLayout *layout, int stream_id, int prev)
{
   int i;
   i = (prev<0) ? 0 : prev+1;
   for (;i<layout->nb_channels;i++)
   {
      if (layout->mapping[i]==2*layout->nb_coupled_streams+stream_id)
         return i;
   }
   return -1;
}

static int validate_encoder_layout(const ChannelLayout *layout)
{
   int s;
   for (s=0;s<layout->nb_streams;s++)
   {
      if (s < layout->nb_coupled_streams)
      {
         if (get_left_channel(layout, s, -1)==-1)
            return 0;
         if (get_right_channel(layout, s, -1)==-1)
            return 0;
      } else {
         if (get_mono_channel(layout, s, -1)==-1)
            return 0;
      }
   }
   return 1;
}

int opus_multistream_encoder_get_size(int nb_streams, int nb_coupled_streams)
{
	int coupled_size;
	int mono_size;

	coupled_size = opus_encoder_get_size(2);
	mono_size = opus_encoder_get_size(1);
	return align(sizeof(OpusMSEncoder))
			+ nb_coupled_streams * align(coupled_size)
	        + (nb_streams-nb_coupled_streams) * align(mono_size);
}



int opus_multistream_encoder_init(
      OpusMSEncoder *st,            /* Encoder state */
      int Fs,                     /* Sampling rate of input signal (Hz) */
      int channels,               /* Number of channels (1/2) in input signal */
      int streams,
      int coupled_streams,
      unsigned char *mapping,
      int application             /* Coding mode (OPUS_APPLICATION_VOIP/OPUS_APPLICATION_AUDIO) */
)
{
   int coupled_size;
   int mono_size;
   int i;
   char *ptr;

   st->layout.nb_channels = channels;
   st->layout.nb_streams = streams;
   st->layout.nb_coupled_streams = coupled_streams;

   for (i=0;i<st->layout.nb_channels;i++)
      st->layout.mapping[i] = mapping[i];
   if (!validate_layout(&st->layout) || !validate_encoder_layout(&st->layout))
      return OPUS_BAD_ARG;
   ptr = (char*)st + align(sizeof(OpusMSEncoder));
   coupled_size = opus_encoder_get_size(2);
   mono_size = opus_encoder_get_size(1);

   for (i=0;i<st->layout.nb_coupled_streams;i++)
   {
      opus_encoder_init((OpusEncoder*)ptr, Fs, 2, application);
      ptr += align(coupled_size);
   }
   for (;i<st->layout.nb_streams;i++)
   {
      opus_encoder_init((OpusEncoder*)ptr, Fs, 1, application);
      ptr += align(mono_size);
   }
   return OPUS_OK;
}

OpusMSEncoder *opus_multistream_encoder_create(
      int Fs,                     /* Sampling rate of input signal (Hz) */
      int channels,               /* Number of channels (1/2) in input signal */
      int streams,
      int coupled_streams,
      unsigned char *mapping,
      int application,            /* Coding mode (OPUS_APPLICATION_VOIP/OPUS_APPLICATION_AUDIO) */
      int *error                  /* Error code */
)
{
   int ret;
   OpusMSEncoder *st = (OpusMSEncoder *)opus_alloc(opus_multistream_encoder_get_size(streams, coupled_streams));
   if (st==NULL)
   {
      if (error)
         *error = OPUS_ALLOC_FAIL;
      return NULL;
   }
   ret = opus_multistream_encoder_init(st, Fs, channels, streams, coupled_streams, mapping, application);
   if (ret != OPUS_OK)
   {
      opus_free(st);
      st = NULL;
   }
   if (error)
      *error = ret;
   return st;
}


#ifdef FIXED_POINT
int opus_multistream_encode(
#else
int opus_multistream_encode_float(
#endif
    OpusMSEncoder *st,            /* Encoder state */
    const opus_val16 *pcm,      /* Input signal (interleaved if 2 channels). length is frame_size*channels */
    int frame_size,             /* Number of samples per frame of input signal */
    unsigned char *data,        /* Output payload (no more than max_data_bytes long) */
    int max_data_bytes          /* Allocated memory for payload; don't use for controlling bitrate */
)
{
   int coupled_size;
   int mono_size;
   int s, i;
   char *ptr;
   int tot_size;
   VARDECL(opus_val16, buf);
   unsigned char tmp_data[1276];
   ALLOC_STACK;

   ALLOC(buf, 2*frame_size, opus_val16);
   ptr = (char*)st + align(sizeof(OpusMSEncoder));
   coupled_size = opus_encoder_get_size(2);
   mono_size = opus_encoder_get_size(1);

   if (max_data_bytes < 2*st->layout.nb_streams-1)
   {
      RESTORE_STACK;
      return OPUS_BUFFER_TOO_SMALL;
   }
   /* Counting ToC */
   tot_size = 0;
   for (s=0;s<st->layout.nb_streams;s++)
   {
      OpusEncoder *enc;
      int len;
      int curr_max;

      enc = (OpusEncoder*)ptr;
      if (s < st->layout.nb_coupled_streams)
      {
         int left, right;
         left = get_left_channel(&st->layout, s, -1);
         right = get_right_channel(&st->layout, s, -1);
         for (i=0;i<frame_size;i++)
         {
            buf[2*i] = pcm[st->layout.nb_channels*i+left];
            buf[2*i+1] = pcm[st->layout.nb_channels*i+right];
         }
         ptr += align(coupled_size);
      } else {
         int chan = get_mono_channel(&st->layout, s, -1);
         for (i=0;i<frame_size;i++)
            buf[i] = pcm[st->layout.nb_channels*i+chan];
         ptr += align(mono_size);
      }
      /* number of bytes left (+Toc) */
      curr_max = max_data_bytes - tot_size;
      /* Reserve one byte for the last stream and 2 for the others */
      curr_max -= 2*(st->layout.nb_streams-s)-1;
      len = opus_encode_native(enc, buf, frame_size, tmp_data, curr_max);
      if (len<0)
      {
         RESTORE_STACK;
         return len;
      }
      /* ToC first */
      *data++ = tmp_data[0];
      if (s != st->layout.nb_streams-1)
      {
         int tmp = encode_size(len-1, data);
         data += tmp;
         tot_size += tmp;
      }
      /* IMPORTANT: Here we assume that the encoder only returned one frame */
      tot_size += len;
      OPUS_COPY(data, &tmp_data[1], len-1);
   }
   RESTORE_STACK;
   return tot_size;

}

#ifdef FIXED_POINT

#ifndef DISABLE_FLOAT_API
int opus_multistream_encode_float(
    OpusMSEncoder *st,          /* Encoder state */
    const float *pcm,           /* Input signal (interleaved if 2 channels). length is frame_size*channels */
    int frame_size,             /* Number of samples per frame of input signal */
    unsigned char *data,        /* Output payload (no more than max_data_bytes long) */
    int max_data_bytes          /* Allocated memory for payload; don't use for controlling bitrate */
)
{
   int i, ret;
   VARDECL(opus_int16, in);
   ALLOC_STACK;

   ALLOC(in, frame_size*st->layout.nb_channels, opus_int16);

   for (i=0;i<frame_size*st->layout.nb_channels;i++)
      in[i] = FLOAT2INT16(pcm[i]);
   ret = opus_multistream_encode(st, in, frame_size, data, max_data_bytes);
   RESTORE_STACK;
   return ret;
}
#endif

#else

int opus_multistream_encode(
    OpusMSEncoder *st,          /* Encoder state */
    const opus_int16 *pcm,      /* Input signal (interleaved if 2 channels). length is frame_size*channels */
    int frame_size,             /* Number of samples per frame of input signal */
    unsigned char *data,        /* Output payload (no more than max_data_bytes long) */
    int max_data_bytes          /* Allocated memory for payload; don't use for controlling bitrate */
)
{
   int i, ret;
   VARDECL(float, in);
   ALLOC_STACK;

   ALLOC(in, frame_size*st->layout.nb_channels, float);

   for (i=0;i<frame_size*st->layout.nb_channels;i++)
      in[i] = (1./32768)*pcm[i];
   ret = opus_multistream_encode_float(st, in, frame_size, data, max_data_bytes);
   RESTORE_STACK;
   return ret;
}

#endif

int opus_multistream_encoder_ctl(OpusMSEncoder *st, int request, ...)
{
   va_list ap;
   int coupled_size, mono_size;
   char *ptr;
   int ret = OPUS_OK;

   va_start(ap, request);

   coupled_size = opus_encoder_get_size(2);
   mono_size = opus_encoder_get_size(1);
   ptr = (char*)st + align(sizeof(OpusMSEncoder));
   switch (request)
   {
   case OPUS_SET_BITRATE_REQUEST:
   {
      int chan, s;
      opus_uint32 value = va_arg(ap, opus_uint32);
      chan = st->layout.nb_streams + st->layout.nb_coupled_streams;
      value /= chan;
      for (s=0;s<st->layout.nb_streams;s++)
      {
         OpusEncoder *enc;
         enc = (OpusEncoder*)ptr;
         opus_encoder_ctl(enc, request, value * (s < st->layout.nb_coupled_streams ? 2 : 1));
      }
   }
   break;
   /* FIXME: Add missing ones */
   case OPUS_GET_BITRATE_REQUEST:
   case OPUS_GET_VBR_REQUEST:
   case OPUS_GET_APPLICATION_REQUEST:
   case OPUS_GET_BANDWIDTH_REQUEST:
   case OPUS_GET_COMPLEXITY_REQUEST:
   case OPUS_GET_PACKET_LOSS_PERC_REQUEST:
   case OPUS_GET_DTX_REQUEST:
   case OPUS_GET_VOICE_RATIO_REQUEST:
   case OPUS_GET_VBR_CONSTRAINT_REQUEST:
   case OPUS_GET_SIGNAL_REQUEST:
   case OPUS_GET_LOOKAHEAD_REQUEST:
   {
      int s;
      /* This works for int32* params */
      opus_uint32 *value = va_arg(ap, opus_uint32*);
      for (s=0;s<st->layout.nb_streams;s++)
      {
         OpusEncoder *enc;

         enc = (OpusEncoder*)ptr;
         if (s < st->layout.nb_coupled_streams)
            ptr += align(coupled_size);
         else
            ptr += align(mono_size);
         ret = opus_encoder_ctl(enc, request, value);
         if (ret < 0)
            break;
      }
   }
   break;
   default:
   {
      int s;
      /* This works for int32 params */
      opus_uint32 value = va_arg(ap, opus_uint32);
      for (s=0;s<st->layout.nb_streams;s++)
      {
         OpusEncoder *enc;

         enc = (OpusEncoder*)ptr;
         if (s < st->layout.nb_coupled_streams)
            ptr += align(coupled_size);
         else
            ptr += align(mono_size);
         ret = opus_encoder_ctl(enc, request, value);
         if (ret < 0)
            break;
      }
   }
   break;
   }

   va_end(ap);
   return ret;
}

void opus_multistream_encoder_destroy(OpusMSEncoder *st)
{
    opus_free(st);
}


/* DECODER */

int opus_multistream_decoder_get_size(int nb_streams, int nb_coupled_streams)
{
   int coupled_size;
   int mono_size;

   coupled_size = opus_decoder_get_size(2);
   mono_size = opus_decoder_get_size(1);
   return align(sizeof(OpusMSDecoder))
         + nb_coupled_streams * align(coupled_size)
         + (nb_streams-nb_coupled_streams) * align(mono_size);
}

int opus_multistream_decoder_init(
      OpusMSDecoder *st,            /* Encoder state */
      int Fs,                     /* Sampling rate of input signal (Hz) */
      int channels,               /* Number of channels (1/2) in input signal */
      int streams,
      int coupled_streams,
      unsigned char *mapping
)
{
   int coupled_size;
   int mono_size;
   int i;
   char *ptr;

   st->layout.nb_channels = channels;
   st->layout.nb_streams = streams;
   st->layout.nb_coupled_streams = coupled_streams;

   for (i=0;i<st->layout.nb_channels;i++)
      st->layout.mapping[i] = mapping[i];
   if (!validate_layout(&st->layout))
      return OPUS_BAD_ARG;

   ptr = (char*)st + align(sizeof(OpusMSEncoder));
   coupled_size = opus_decoder_get_size(2);
   mono_size = opus_decoder_get_size(1);

   for (i=0;i<st->layout.nb_coupled_streams;i++)
   {
      opus_decoder_init((OpusDecoder*)ptr, Fs, 2);
      ptr += align(coupled_size);
   }
   for (;i<st->layout.nb_streams;i++)
   {
      opus_decoder_init((OpusDecoder*)ptr, Fs, 1);
      ptr += align(mono_size);
   }
   return OPUS_OK;
}


OpusMSDecoder *opus_multistream_decoder_create(
      int Fs,                     /* Sampling rate of input signal (Hz) */
      int channels,               /* Number of channels (1/2) in input signal */
      int streams,
      int coupled_streams,
      unsigned char *mapping,
      int *error                  /* Error code */
)
{
   int ret;
   OpusMSDecoder *st = (OpusMSDecoder *)opus_alloc(opus_multistream_decoder_get_size(streams, coupled_streams));
   if (st==NULL)
   {
      if (error)
         *error = OPUS_ALLOC_FAIL;
      return NULL;
   }
   ret = opus_multistream_decoder_init(st, Fs, channels, streams, coupled_streams, mapping);
   if (error)
      *error = ret;
   if (ret != OPUS_OK)
   {
      opus_free(st);
      st = NULL;
   }
   return st;


}

static int opus_multistream_decode_native(
      OpusMSDecoder *st,            /* Encoder state */
      const unsigned char *data,
      int len,
      opus_val16 *pcm,
      int frame_size,
      int decode_fec
)
{
   int coupled_size;
   int mono_size;
   int s, i, c;
   char *ptr;
   VARDECL(opus_val16, buf);
   ALLOC_STACK;

   ALLOC(buf, 2*frame_size, opus_val16);
   ptr = (char*)st + align(sizeof(OpusMSEncoder));
   coupled_size = opus_decoder_get_size(2);
   mono_size = opus_decoder_get_size(1);

   if (len < 2*st->layout.nb_streams-1)
      return OPUS_BUFFER_TOO_SMALL;
   for (s=0;s<st->layout.nb_streams;s++)
   {
      OpusDecoder *dec;
      int packet_offset, ret;

      dec = (OpusDecoder*)ptr;
      ptr += (s < st->layout.nb_coupled_streams) ? align(coupled_size) : align(mono_size);

      if (len<=0)
      {
         RESTORE_STACK;
         return OPUS_CORRUPTED_DATA;
      }
      ret = opus_decode_native(dec, data, len, buf, frame_size, decode_fec, 1, &packet_offset);
      data += packet_offset;
      len -= packet_offset;
      if (ret > frame_size)
      {
         RESTORE_STACK;
         return OPUS_BUFFER_TOO_SMALL;
      }
      if (s>0 && ret != frame_size)
      {
         RESTORE_STACK;
         return OPUS_CORRUPTED_DATA;
      }
      if (ret <= 0)
      {
         RESTORE_STACK;
         return ret;
      }
      frame_size = ret;
      if (s < st->layout.nb_coupled_streams)
      {
         int chan, prev;
         prev = -1;
         /* Copy "left" audio to the channel(s) where it belongs */
         while ( (chan = get_left_channel(&st->layout, s, prev)) != -1)
         {
            for (i=0;i<frame_size;i++)
               pcm[st->layout.nb_channels*i+chan] = buf[2*i];
            prev = chan;
         }
         prev = -1;
         /* Copy "right" audio to the channel(s) where it belongs */
         while ( (chan = get_right_channel(&st->layout, s, prev)) != -1)
         {
            for (i=0;i<frame_size;i++)
               pcm[st->layout.nb_channels*i+chan] = buf[2*i+1];
            prev = chan;
         }
      } else {
         int chan, prev;
         prev = -1;
         /* Copy audio to the channel(s) where it belongs */
         while ( (chan = get_mono_channel(&st->layout, s, prev)) != -1)
         {
            for (i=0;i<frame_size;i++)
               pcm[st->layout.nb_channels*i+chan] = buf[i];
            prev = chan;
         }
      }
   }
   /* Handle muted channels */
   for (c=0;c<st->layout.nb_channels;c++)
   {
      if (st->layout.mapping[c] == 255)
      {
         for (i=0;i<frame_size;i++)
            pcm[st->layout.nb_channels*i+c] = 0;
      }
   }
   RESTORE_STACK;
   return frame_size;
}

#ifdef FIXED_POINT
int opus_multistream_decode(
      OpusMSDecoder *st,            /* Encoder state */
      const unsigned char *data,
      int len,
      opus_int16 *pcm,
      int frame_size,
      int decode_fec
)
{
   return opus_multistream_decode_native(st, data, len, pcm, frame_size, decode_fec);
}

#ifndef DISABLE_FLOAT_API
int opus_multistream_decode_float(OpusMSDecoder *st, const unsigned char *data,
      int len, float *pcm, int frame_size, int decode_fec)
{
   VARDECL(opus_int16, out);
   int ret, i;
   ALLOC_STACK;

   ALLOC(out, frame_size*st->layout.nb_channels, opus_int16);

   ret = opus_multistream_decode_native(st, data, len, out, frame_size, decode_fec);
   if (ret > 0)
   {
      for (i=0;i<ret*st->layout.nb_channels;i++)
         pcm[i] = (1./32768.)*(out[i]);
   }
   RESTORE_STACK;
   return ret;
}
#endif

#else

int opus_multistream_decode(OpusMSDecoder *st, const unsigned char *data,
      int len, opus_int16 *pcm, int frame_size, int decode_fec)
{
   VARDECL(float, out);
   int ret, i;
   ALLOC_STACK;

   ALLOC(out, frame_size*st->layout.nb_channels, float);

   ret = opus_multistream_decode_native(st, data, len, out, frame_size, decode_fec);
   if (ret > 0)
   {
      for (i=0;i<ret*st->layout.nb_channels;i++)
         pcm[i] = FLOAT2INT16(out[i]);
   }
   RESTORE_STACK;
   return ret;
}

int opus_multistream_decode_float(
      OpusMSDecoder *st,            /* Encoder state */
      const unsigned char *data,
      int len,
      float *pcm,
      int frame_size,
      int decode_fec
)
{
   return opus_multistream_decode_native(st, data, len, pcm, frame_size, decode_fec);
}
#endif

int opus_multistream_decoder_ctl(OpusMSDecoder *st, int request, ...)
{
   va_list ap;
   int coupled_size, mono_size;
   char *ptr;
   int ret = OPUS_OK;

   va_start(ap, request);

   coupled_size = opus_decoder_get_size(2);
   mono_size = opus_decoder_get_size(1);
   ptr = (char*)st + align(sizeof(OpusMSDecoder));
   switch (request)
   {
       default:
       {
          int s;
          /* This only works for int32* params, but that's all we have right now */
          opus_uint32 *value = va_arg(ap, opus_uint32*);
          for (s=0;s<st->layout.nb_streams;s++)
          {
             OpusDecoder *enc;

             enc = (OpusDecoder*)ptr;
             if (s < st->layout.nb_coupled_streams)
                ptr += align(coupled_size);
             else
                ptr += align(mono_size);
             ret = opus_decoder_ctl(enc, request, value);
             if (ret < 0)
                break;
          }
       }
       break;
   }

   va_end(ap);
   return ret;
}


void opus_multistream_decoder_destroy(OpusMSDecoder *st)
{
    opus_free(st);
}