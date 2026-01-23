#define NAPI_VERSION 10
#include "utils.h"
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <node_api.h>
#include <windows.h>

typedef volatile struct {
  char *url;
  HANDLE sem;
  HANDLE thread;
  napi_ref connection;
  napi_threadsafe_function jsPlay;
  bool paused;
  bool stopped;
} PlaybackState;

static inline void writePacket(AVPacket *pkt, PlaybackState *state) {
  if (!state->paused && !state->stopped) {
    if (pkt->duration != 960)
      printf("WARNING: unexpected duration %lld\n", pkt->duration);
    napi_call_threadsafe_function(state->jsPlay, pkt, napi_tsfn_nonblocking);
    WaitForSingleObject(state->sem, INFINITE);
  }
  av_packet_unref(pkt);
}
static inline void printDict(const AVDictionary *m) {
  int err;
  char *entries;

  CHECK_ERR(av_dict_get_string(m, &entries, ':', '\n'),
            "Failed to obtain dict entries");
  if (*entries != 0)
    printf("%s\n", entries);
}
static inline void openInput(AVFormatContext *ic, char *url) {
  int err;
  AVDictionary *options = NULL;

  ic->skip_estimate_duration_from_pts = 1;
  CHECK_ERR(av_dict_set(&options, "reconnect", "1", 0),
            "Couldn't set reconnect option");
  CHECK_ERR(av_dict_set(&options, "reconnect_at_eof", "1", 0),
            "Couldn't set reconnect at eof option");
  CHECK_ERR(av_dict_set(&options, "reconnect_on_network_error", "1", 0),
            "Couldn't set reconnect on network error option");
  CHECK_ERR(av_dict_set(&options, "reconnect_streamed", "1", 0),
            "Couldn't set reconnect streamed option");
  CHECK_ERR(av_dict_set(&options, "reconnect_max_retries", "4", 0),
            "Couldn't set reconnect max retries option");
  CHECK_ERR(avformat_open_input(&ic, url, NULL, &options),
            "Could not open input");
  if (av_dict_count(options) > 0) {
    printf("Couldn't set invalid format options:\n");
    printDict(options);
    exit(1);
  }
}
static inline void findAudioStream(AVFormatContext *ic, int *streamNumber,
                                   AVCodecContext **decoderContext) {
  int err;
  const AVCodec *decoder;

  CHECK_ERR(avformat_find_stream_info(ic, NULL), "Could not find stream info");
  CHECK_ERR(*streamNumber = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1,
                                                &decoder, 0),
            "Couldn't find an audio stream");
  printf("Found stream %d (%s)\n", *streamNumber, decoder->long_name);

  // Log stream metadata
  printDict(ic->streams[*streamNumber]->metadata);
  printDict(ic->metadata);

  // Open decoder
  assert(*decoderContext = avcodec_alloc_context3(decoder));
  CHECK_ERR(avcodec_parameters_to_context(*decoderContext,
                                          ic->streams[*streamNumber]->codecpar),
            "Could not copy decoder params");
  CHECK_ERR(avcodec_open2(*decoderContext, decoder, NULL),
            "Could not open decoder");
}
DWORD WINAPI ffmpegThread(LPVOID data) {
  PlaybackState *state = data;
  int err, streamNumber;
  int64_t pts = 0;
  AVCodecContext *decoderContext,
      *encoderContext =
          avcodec_alloc_context3(avcodec_find_encoder_by_name("libopus"));
  AVFrame *inputFrame = av_frame_alloc(), *outputFrame = av_frame_alloc();
  AVFormatContext *ic = avformat_alloc_context();
  AVPacket *pkt = av_packet_alloc();
  SwrContext *s = NULL;

  // Initialize
  av_log_set_level(AV_LOG_INFO);
  assert(pkt && inputFrame && outputFrame && ic);
  printf("Opening input\n");

  // Open input
  openInput(ic, state->url);
  printf("Opened input %s\n", state->url);
  free(state->url);
  state->url = NULL;

  // Find audio stream
  findAudioStream(ic, &streamNumber, &decoderContext);
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
  CHECK_ERR(avcodec_open2(encoderContext, NULL, NULL),
            "Could not open encoder");
  printf("Opened encoder\n");

  // Read frames
  while (!state->stopped && (err = av_read_frame(ic, pkt)) != AVERROR_EOF) {
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
          writePacket(pkt, state);
      }
    }
    if (err != AVERROR(EAGAIN))
      CHECK_ERR(err, "Failed to recode frame");
  }

  // Flush encoder
  printf("Flushing encoder\n");
  avcodec_send_frame(encoderContext, NULL);
  while (avcodec_receive_packet(encoderContext, pkt) >= 0)
    writePacket(pkt, state);

  // Close and free
  av_frame_free(&inputFrame);
  av_frame_free(&outputFrame);
  av_packet_free(&pkt);
  avformat_close_input(&ic);
  avcodec_free_context(&decoderContext);
  swr_free(&s);
  avcodec_free_context(&encoderContext);
  printf("Thread is closing\n");
  return 0;
}

static void playOpusPacket(napi_env env, napi_value jsPlayOpusPacket,
                           void *context, void *data) {
  PlaybackState *state = context;
  AVPacket *pkt = data;
  napi_value recv;
  napi_value buffer;

  NODE_API_CALL_DEFAULT(
      napi_get_reference_value(env, state->connection, &recv), );
  NODE_API_CALL_DEFAULT(napi_create_external_buffer(env, pkt->size, pkt->data,
                                                    NULL, NULL, &buffer), );
  NODE_API_CALL_DEFAULT(
      napi_call_function(env, recv, jsPlayOpusPacket, 1, &buffer, NULL), );
  ReleaseSemaphore(state->sem, 1, NULL);
}
static napi_value destroy(napi_env env, napi_callback_info cbinfo) {
  size_t argc = 2;
  napi_value arguments[2];
  napi_value this;
  PlaybackState *state;

  NODE_API_CALL(napi_get_cb_info(env, cbinfo, &argc, arguments, &this, NULL));
  NODE_API_CALL(napi_unwrap(env, this, (void **)&state));
  free(state->url);
  state->stopped = true;
  state->url = NULL;
  ReleaseSemaphore(state->sem, 1, NULL);
  if ((WaitForSingleObject(state->thread, parseInt(env, arguments[0], 1,
                                                   20000)) == WAIT_TIMEOUT) &&
      parseBool(env, arguments[1], 0))
    TerminateThread(state->thread, 2033);
  CloseHandle(state->thread);
  CloseHandle(state->sem);
  NODE_API_CALL(napi_delete_reference(env, state->connection));
  NODE_API_CALL(
      napi_release_threadsafe_function(state->jsPlay, napi_tsfn_abort));
  free((void *)state);
  return UNDEFINED;
}
static napi_value stop(napi_env env, napi_callback_info cbinfo) {
  size_t argc = 2;
  napi_value arguments[2];
  napi_value this;
  PlaybackState *state;

  NODE_API_CALL(napi_get_cb_info(env, cbinfo, &argc, arguments, &this, NULL));
  NODE_API_CALL(napi_unwrap(env, this, (void **)&state));
  free(state->url);
  state->stopped = true;
  state->url = NULL;
  ReleaseSemaphore(state->sem, 1, NULL);
  if ((WaitForSingleObject(state->thread, parseInt(env, arguments[0], 1,
                                                   20000)) == WAIT_TIMEOUT) &&
      parseBool(env, arguments[1], 0))
    TerminateThread(state->thread, 2033);
  CloseHandle(state->thread);
  state->thread = NULL;
  return UNDEFINED;
}
static napi_value play(napi_env env, napi_callback_info cbinfo) {
  size_t argc = 1;
  napi_value arguments[1];
  napi_value this;
  PlaybackState *state;

  NODE_API_CALL(napi_get_cb_info(env, cbinfo, &argc, arguments, &this, NULL));
  NODE_API_CALL(napi_unwrap(env, this, (void **)&state));
  state->url = parseString(env, arguments[0]);
  state->paused = false;
  state->stopped = false;
  state->thread = CreateThread(NULL, 0, ffmpegThread, (void *)state, 0, NULL);
  return UNDEFINED;
}
static napi_value createPlayer(napi_env env, napi_callback_info cbinfo) {
  size_t argc = 1;
  napi_value arguments[1];
  napi_value jsPlayOpusPacket;
  napi_value this;
  PlaybackState *state = malloc(sizeof(PlaybackState));

  state->paused = false;
  state->stopped = true;
  state->thread = NULL;
  state->url = NULL;
  state->sem = CreateSemaphoreA(NULL, 0, 1, NULL);
  NODE_API_CALL(napi_get_cb_info(env, cbinfo, &argc, arguments, &this, NULL));
  NODE_API_CALL(napi_create_reference(env, arguments[0], 1,
                                      (napi_ref *)&state->connection));
  NODE_API_CALL(napi_get_named_property(env, arguments[0], "playOpusPacket",
                                        &jsPlayOpusPacket));
  NODE_API_CALL(napi_create_threadsafe_function(
      env, jsPlayOpusPacket, NULL, STRING("playOpusPacket"), 1, 1, NULL, NULL,
      (void *)state, playOpusPacket,
      (napi_threadsafe_function *)&state->jsPlay));
  NODE_API_CALL(napi_wrap(env, this, (void *)state, NULL, NULL, NULL));
  return this;
}

NAPI_MODULE_INIT(/* napi_env env, napi_value exports */) {
  napi_value AudioPlayer;
  napi_property_descriptor properties[] = {
      {.utf8name = "play", .method = play},
      {.utf8name = "stop", .method = stop},
      {.utf8name = "destroy", .method = destroy}};

  NODE_API_CALL(napi_define_class(
      env, "AudioPlayer", NAPI_AUTO_LENGTH, createPlayer, NULL,
      sizeof(properties) / sizeof(properties[0]), properties, &AudioPlayer));
  NODE_API_CALL(
      napi_set_named_property(env, exports, "AudioPlayer", AudioPlayer));
  return exports;
}
