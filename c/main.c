#include "libavutil/rational.h"
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

#define BITRATE 380000
#define BUFFERING_TIME 320
#define DEFAULT_TIMEOUT 20000
#define FRAME_SIZE 960
#define SAMPLE_RATE 48000
#define TERM_CODE 2033

LARGE_INTEGER freq;

typedef volatile struct {
  LARGE_INTEGER start;
  char *url;
  HANDLE sem;
  HANDLE thread;
  napi_ref connection;
  napi_threadsafe_function jsPlay;
  bool paused;
  bool stopped;
} PlaybackState;

static inline void writePacket(AVPacket *pkt, PlaybackState *state) {
  LARGE_INTEGER now;

  napi_call_threadsafe_function(state->jsPlay, pkt, napi_tsfn_nonblocking);
  if (pkt->duration != FRAME_SIZE)
    printf("WARNING: unexpected duration %lld\n", pkt->duration);
  QueryPerformanceCounter(&now);
  if ((now.QuadPart =
           pkt->pts / (SAMPLE_RATE / 1000) -
           (now.QuadPart - state->start.QuadPart) * 1000 / freq.QuadPart -
           BUFFERING_TIME) > 0)
    Sleep(now.QuadPart);
  napi_call_threadsafe_function(state->jsPlay, NULL, napi_tsfn_nonblocking);
  WaitForSingleObject(state->sem, INFINITE);
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
static inline void openInput(AVFormatContext *ic, PlaybackState *state) {
  int err;
  AVDictionary *options = NULL;

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
  CHECK_ERR(avformat_open_input(&ic, state->url, NULL, &options),
            "Could not open input");
  QueryPerformanceCounter((LARGE_INTEGER *)&state->start);
  free(state->url);
  state->url = NULL;
  if (av_dict_count(options) > 0) {
    printf("WARNING: Couldn't set invalid format options:\n");
    printDict(options);
  }
}
static inline void findAudioStream(AVFormatContext *ic, int *streamNumber,
                                   AVCodecContext **decoderContext) {
  int err;
  const AVCodec *decoder;
  const AVStream *stream;

  CHECK_ERR(avformat_find_stream_info(ic, NULL), "Could not find stream info");
  CHECK_ERR(*streamNumber = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1,
                                                &decoder, 0),
            "Couldn't find an audio stream");
  stream = ic->streams[*streamNumber];
  printf("Found stream %d (%s)\n", *streamNumber, decoder->long_name);

  // Log stream metadata
  printDict(stream->metadata);
  printDict(ic->metadata);

  // Check if recoding is needed
  if (decoder->id == AV_CODEC_ID_OPUS &&
      stream->codecpar->bit_rate <= BITRATE &&
      stream->codecpar->ch_layout.nb_channels == 2 &&
      stream->codecpar->ch_layout.order == AV_CHANNEL_ORDER_NATIVE &&
      stream->codecpar->sample_rate == SAMPLE_RATE &&
      (stream->codecpar->frame_size == FRAME_SIZE ||
       stream->codecpar->frame_size == 0) &&
      stream->time_base.num == 1 && stream->time_base.den == SAMPLE_RATE)
    printf("INFO: Found compatible stream, skipping recoding\n");
  else {
    // Open decoder
    assert(*decoderContext = avcodec_alloc_context3(decoder));
    CHECK_ERR(avcodec_parameters_to_context(*decoderContext, stream->codecpar),
              "Could not copy decoder params");
    CHECK_ERR(avcodec_open2(*decoderContext, decoder, NULL),
              "Could not open decoder");
  }
  printf("Bitrate: %lld\nSample rate: %d\nChannels: %d\nTime base: "
         "%d/%d\nFrame size: %d\n",
         stream->codecpar->bit_rate, stream->codecpar->sample_rate,
         stream->codecpar->ch_layout.nb_channels, stream->time_base.num,
         stream->time_base.den, stream->codecpar->frame_size);
}
DWORD WINAPI ffmpegThread(LPVOID data) {
  PlaybackState *state = data;
  int err, streamNumber;
  AVFormatContext *ic = avformat_alloc_context();
  AVPacket *pkt = av_packet_alloc();
  // These are needed only when recoding
  AVCodecContext *decoderContext = NULL, *encoderContext = NULL;
  AVFrame *inputFrame = NULL, *outputFrame = NULL;
  SwrContext *s = NULL;

  // Initialize
  av_log_set_level(AV_LOG_INFO);
  assert(pkt && ic);
  printf("Opening input %s\n", state->url);

  // Open input
  openInput(ic, state);
  printf("Opened input\n");

  // Find audio stream
  findAudioStream(ic, &streamNumber, &decoderContext);

  if (decoderContext) {
    // Set encoder options
    encoderContext =
        avcodec_alloc_context3(avcodec_find_encoder_by_name("libopus"));
    encoderContext->bit_rate = BITRATE;
    encoderContext->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    encoderContext->sample_fmt = AV_SAMPLE_FMT_FLT;
    encoderContext->time_base =
        (AVRational){1, (encoderContext->sample_rate = SAMPLE_RATE)};
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
    inputFrame = av_frame_alloc();
    outputFrame = av_frame_alloc();
  }

  // Read frames
  while (!state->stopped && (err = av_read_frame(ic, pkt)) != AVERROR_EOF) {
    CHECK_ERR(err, "Error reading frame");
    if (pkt->stream_index != streamNumber) {
      av_packet_unref(pkt);
      continue;
    }
    if (decoderContext) {
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

        while ((err = swr_get_out_samples(s, 0)) >=
               encoderContext->frame_size) {
          // Set resampled frame parameters
          outputFrame->format = encoderContext->sample_fmt;
          outputFrame->nb_samples = encoderContext->frame_size;
          outputFrame->sample_rate = encoderContext->sample_rate;
          outputFrame->pts = swr_next_pts(s, INT64_MIN) / SAMPLE_RATE;
          av_channel_layout_copy(&outputFrame->ch_layout,
                                 &encoderContext->ch_layout);

          // Read resampled frames
          CHECK_ERR(av_frame_get_buffer(outputFrame, 0),
                    "Failed to get buffer");
          CHECK_ERR(swr_convert(s, outputFrame->extended_data,
                                outputFrame->nb_samples, NULL, 0),
                    "Resampling failed");

          // Encode frame
          CHECK_ERR(avcodec_send_frame(encoderContext, outputFrame),
                    "Error sending frame to encoder");
          av_frame_unref(outputFrame);

          // Receive encoded packets
          while ((err = avcodec_receive_packet(encoderContext, pkt)) >= 0)
            writePacket(pkt, state);
        }
      }
      if (err != AVERROR(EAGAIN))
        CHECK_ERR(err, "Failed to recode frame");
    } else
      writePacket(pkt, state);
  }

  state->stopped = true;
  if (encoderContext) {
    // Flush encoder
    printf("Flushing encoder\n");
    avcodec_send_frame(encoderContext, NULL);
    while (avcodec_receive_packet(encoderContext, pkt) >= 0)
      writePacket(pkt, state);
  }

  // Close and free
  printf("Freeing resources\n");
  av_packet_free(&pkt);
  avformat_close_input(&ic);
  if (encoderContext) {
    av_frame_free(&inputFrame);
    av_frame_free(&outputFrame);
    avcodec_free_context(&decoderContext);
    avcodec_free_context(&encoderContext);
    swr_free(&s);
  }
  printf("Closing thread\n");
  CloseHandle(state->thread);
  state->thread = NULL;
  return 0;
}

static void playOpusPacket(napi_env env, napi_value jsFn, void *context,
                           void *data) {
  PlaybackState *state = context;
  napi_value recv;

  NODE_API_CALL_DEFAULT(
      napi_get_reference_value(env, state->connection, &recv), );
  if (data) {
    AVPacket *pkt = data;
    napi_value buffer;

    NODE_API_CALL_DEFAULT(napi_create_external_buffer(env, pkt->size, pkt->data,
                                                      NULL, NULL, &buffer), );
    NODE_API_CALL_DEFAULT(
        napi_get_named_property(env, recv, "prepareAudioPacket", &jsFn), );
    NODE_API_CALL_DEFAULT(
        napi_call_function(env, recv, jsFn, 1, &buffer, NULL), );
  } else {
    NODE_API_CALL_DEFAULT(napi_call_function(env, recv, jsFn, 0, NULL, NULL), );
    ReleaseSemaphore(state->sem, 1, NULL);
  }
}
static napi_value destroy(napi_env env, napi_callback_info cbinfo) {
  size_t argc = 2;
  napi_value arguments[2];
  napi_value this;
  PlaybackState *state;

  NODE_API_CALL(napi_get_cb_info(env, cbinfo, &argc, arguments, &this, NULL));
  NODE_API_CALL(napi_unwrap(env, this, (void **)&state));
  if (!state)
    return UNDEFINED;
  free(state->url);
  state->stopped = true;
  state->url = NULL;
  ReleaseSemaphore(state->sem, 1, NULL);
  if ((WaitForSingleObject(state->thread,
                           parseInt(env, arguments[0], 1, DEFAULT_TIMEOUT)) ==
       WAIT_TIMEOUT) &&
      parseBool(env, arguments[1], true))
    TerminateThread(state->thread, TERM_CODE);
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
  if (!state) {
    NODE_API_CALL(napi_throw_error(env, NULL, "Player is destroyed"));
    return UNDEFINED;
  }
  if (state->stopped)
    return UNDEFINED;
  STOP(parseInt(env, arguments[0], 1, DEFAULT_TIMEOUT),
       parseBool(env, arguments[1], false));
  return UNDEFINED;
}
// TODO: Implement pause()
static napi_value play(napi_env env, napi_callback_info cbinfo) {
  size_t argc = 1;
  napi_value arguments[1];
  napi_value this;
  PlaybackState *state;

  NODE_API_CALL(napi_get_cb_info(env, cbinfo, &argc, arguments, &this, NULL));
  NODE_API_CALL(napi_unwrap(env, this, (void **)&state));
  if (!state) {
    NODE_API_CALL(napi_throw_error(env, NULL, "Player is destroyed"));
    return UNDEFINED;
  }
  if (!state->stopped)
    STOP(DEFAULT_TIMEOUT, true);
  state->url = parseString(env, arguments[0]);
  state->paused = false;
  state->stopped = false;
  state->thread = CreateThread(NULL, 0, ffmpegThread, (void *)state, 0, NULL);
  return UNDEFINED;
}
static napi_value createPlayer(napi_env env, napi_callback_info cbinfo) {
  size_t argc = 1;
  napi_value arguments[1];
  napi_value dispatchAudio;
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
  NODE_API_CALL(napi_get_named_property(env, arguments[0], "dispatchAudio",
                                        &dispatchAudio));
  NODE_API_CALL(napi_create_threadsafe_function(
      env, dispatchAudio, NULL, STRING("dispatchAudio"), 2, 1, NULL, NULL,
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

  QueryPerformanceFrequency(&freq);
  NODE_API_CALL(napi_define_class(
      env, "AudioPlayer", NAPI_AUTO_LENGTH, createPlayer, NULL,
      sizeof(properties) / sizeof(properties[0]), properties, &AudioPlayer));
  NODE_API_CALL(
      napi_set_named_property(env, exports, "AudioPlayer", AudioPlayer));
  return exports;
}
