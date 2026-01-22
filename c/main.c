#include "utils.h"
#include <assert.h>
#include <corecrt_search.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <node_api.h>
#include <windows.h>

static napi_threadsafe_function func;
HANDLE sem;
volatile char ok = 1;

static inline void writePacket(AVPacket *pkt) {
  if (ok) {
    if (pkt->duration != 960)
      printf("WARNING: unexpected duration %lld\n", pkt->duration);
    napi_call_threadsafe_function(func, pkt, napi_tsfn_nonblocking);
    WaitForSingleObject(sem, INFINITE);
  }
  av_packet_unref(pkt);
}
DWORD WINAPI ffmpegThread(LPVOID url) {
  int err, streamNumber;
  int64_t pts = 0;
  const AVCodec *decoder, *encoder = avcodec_find_encoder_by_name("libopus");
  AVCodecContext *decoderContext,
      *encoderContext = avcodec_alloc_context3(encoder);
  AVFrame *inputFrame = av_frame_alloc(), *outputFrame = av_frame_alloc();
  AVFormatContext *ic = avformat_alloc_context();
  AVPacket *pkt = av_packet_alloc();
  SwrContext *s = NULL;

  // Initialize
  av_log_set_level(AV_LOG_INFO);
  assert(pkt && inputFrame && outputFrame && encoder && ic && encoderContext);

  // Open input
  ic->skip_estimate_duration_from_pts = 1;
  CHECK_ERR(avformat_open_input(&ic, url, NULL, NULL), "Could not open input");
  printf("Opened input %s\n", (char *)url);
  free(url);

  // Find audio stream
  CHECK_ERR(avformat_find_stream_info(ic, NULL), "Could not find stream info");
  CHECK_ERR(streamNumber = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1,
                                               &decoder, 0),
            "Couldn't find an audio stream");
  printf("Found stream %d (%s)\n", streamNumber, decoder->long_name);
  {
    const AVDictionaryEntry *prev = NULL;

    while ((prev = av_dict_iterate(ic->streams[streamNumber]->metadata, prev)))
      printf("%s = %s\n", prev->key, prev->value);
    prev = NULL;
    while ((prev = av_dict_iterate(ic->metadata, prev)))
      printf("%s = %s\n", prev->key, prev->value);
  }

  // Open decoder
  assert(decoderContext = avcodec_alloc_context3(decoder));
  CHECK_ERR(avcodec_parameters_to_context(decoderContext,
                                          ic->streams[streamNumber]->codecpar),
            "Could not copy decoder params");
  CHECK_ERR(avcodec_open2(decoderContext, decoder, NULL),
            "Could not open decoder");
  printf("Bitrate: %lld\nSample rate: %d\nChannels: %d\nTime base: "
         "%d/%d\nSample format: %d\n",
         decoderContext->bit_rate, decoderContext->sample_rate,
         decoderContext->ch_layout.nb_channels, decoderContext->time_base.num,
         decoderContext->time_base.den, decoderContext->sample_fmt);

  // Set encoder options
  encoderContext->bit_rate = 256000;
  encoderContext->ch_layout.nb_channels = 2;
  encoderContext->ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
  encoderContext->ch_layout.u.mask = AV_CH_LAYOUT_STEREO;
  encoderContext->sample_fmt = AV_SAMPLE_FMT_FLT;
  encoderContext->time_base =
      (AVRational){1, (encoderContext->sample_rate = 48000)};
  CHECK_ERR(av_opt_set(encoderContext->priv_data, "vbr", "off", 0),
            "Failed to enable cbr");

  // Initialize resampler
  CHECK_ERR(swr_alloc_set_opts2(
                &s, &encoderContext->ch_layout, encoderContext->sample_fmt,
                encoderContext->sample_rate, &decoderContext->ch_layout,
                decoderContext->sample_fmt, decoderContext->sample_rate, 0,
                NULL),
            "Could not set resampler options");
  CHECK_ERR(swr_init(s), "Could not initialize resampler");
  printf("Initialized resampler\n");

  // Open encoder
  CHECK_ERR(avcodec_open2(encoderContext, encoder, NULL),
            "Could not open encoder");
  printf("Opened encoder\n");

  // Read frames
  while (ok && (err = av_read_frame(ic, pkt)) != AVERROR_EOF) {
    CHECK_ERR(err, "Error reading frame");
    if (pkt->stream_index != streamNumber) {
      av_packet_unref(pkt);
      continue;
    }

    // Decode packet
    CHECK_ERR(avcodec_send_packet(decoderContext, pkt),
              "Error sending packet to decoder");
    av_packet_unref(pkt);

    // Receive decoded frames
    while ((err = avcodec_receive_frame(decoderContext, inputFrame)) >= 0) {
      // Send frames to resampler
      CHECK_ERR(swr_convert(s, NULL, 0,
                            (const uint8_t **)inputFrame->extended_data,
                            inputFrame->nb_samples),
                "Error sending frame to resampler");
      av_frame_unref(inputFrame);

      while ((err = swr_get_out_samples(s, 0)) >= encoderContext->frame_size) {
        // Set resampled frame parameters
        outputFrame->format = encoderContext->sample_fmt;
        outputFrame->nb_samples = encoderContext->frame_size;
        outputFrame->pts = pts;
        outputFrame->sample_rate = encoderContext->sample_rate;
        av_channel_layout_copy(&outputFrame->ch_layout,
                               &encoderContext->ch_layout);

        // Read resampled frames
        CHECK_ERR(av_frame_get_buffer(outputFrame, 0), "Failed to get buffer");
        CHECK_ERR(swr_convert(s, outputFrame->extended_data,
                              outputFrame->nb_samples, NULL, 0),
                  "Resampling failed");

        // Encode frame
        CHECK_ERR(avcodec_send_frame(encoderContext, outputFrame),
                  "Error sending frame to encoder");
        pts += outputFrame->nb_samples;
        av_frame_unref(outputFrame);

        // Receive encoded packets
        while ((err = avcodec_receive_packet(encoderContext, pkt)) >= 0)
          writePacket(pkt);
      }
    }
    if (err != AVERROR(EAGAIN))
      CHECK_ERR(err, "Failed to recode frame");
  }

  // Flush encoder
  avcodec_send_frame(encoderContext, NULL);
  while (avcodec_receive_packet(encoderContext, pkt) >= 0)
    writePacket(pkt);

  // Close and free
  av_frame_free(&inputFrame);
  av_frame_free(&outputFrame);
  av_packet_free(&pkt);
  avformat_close_input(&ic);
  avcodec_free_context(&decoderContext);
  swr_free(&s);
  avcodec_free_context(&encoderContext);
  return 0;
}

static void playOpusPacket(napi_env env, napi_value jsPlayOpusPacket,
                           void *context, void *data) {
  napi_value connection;
  napi_value argv;
  AVPacket *pkt = data;

  NODE_API_CALL_DEFAULT(napi_get_reference_value(env, context, &connection), );
  NODE_API_CALL_DEFAULT(napi_create_external_buffer(env, pkt->size, pkt->data,
                                                    NULL, NULL, &argv), );
  NODE_API_CALL_DEFAULT(
      napi_call_function(env, connection, jsPlayOpusPacket, 1, &argv, NULL), );
  ReleaseSemaphore(sem, 1, NULL);
}
static napi_value stop(napi_env env, napi_callback_info cbinfo) {
  ReleaseSemaphore(sem, INFINITE, NULL);
  ok = 0;
  NODE_API_CALL(napi_release_threadsafe_function(func, napi_tsfn_abort));
  return UNDEFINED;
}
static napi_value play(napi_env env, napi_callback_info cbinfo) {
  NODE_LOAD_ARGUMENTS(2, cbinfo);
  char *url = parseString(env, arguments[0]);
  napi_value jsPlayOpusPacket;
  napi_ref context;

  NODE_API_CALL(napi_get_named_property(env, arguments[1], "playOpusPacket",
                                        &jsPlayOpusPacket));
  NODE_API_CALL(napi_create_reference(env, arguments[1], 0, &context));
  NODE_API_CALL(napi_create_threadsafe_function(
      env, jsPlayOpusPacket, NULL, arguments[0], 1, 1, NULL, NULL, context,
      playOpusPacket, &func));
  sem = CreateSemaphoreA(NULL, 0, 1, NULL);
  CreateThread(NULL, 0, ffmpegThread, url, 0, NULL);
  return FUNCTION(stop);
}

NAPI_MODULE_INIT(/* napi_env env, napi_value exports */) {
  EXPORT_FN(play);
  return exports;
}
