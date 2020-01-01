/*
 * Copyright (C) 2019 Oliver Wiedemann
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "codec.h"

static void set_codec_options(AVDictionary **opt, enc_id id)
{
	switch (id) {
	case LIBX264:
		av_dict_set(opt, "preset", "ultrafast", 0);
		av_dict_set(opt, "tune", "zerolatency", 0);
		av_dict_set(opt, "aq-mode", "autovariance", 0);
		av_dict_set(opt, "gop-size", "3", 0);
		break;
	case LIBX265:
		av_dict_set(opt, "preset", "ultrafast", 0);
		av_dict_set(opt, "tune", "zerolatency", 0);
		av_dict_set(opt, "x265-params", "aq-mode=2", 0); //autovariance
		av_dict_set(opt, "gop-size", "3", 0);
		break;
	default:
		pexit("trying to set options for unsupported codec");
	}
}

encoder_context *encoder_init(enc_id id, decoder_context *dc, window_context *wc)
{
	encoder_context *ec;
	AVCodecContext *avctx;
	AVCodec *codec;
	AVDictionary *options = NULL;

	ec = malloc(sizeof(encoder_context));
	if (!ec)
		pexit("malloc failed");

	switch (id) {
	case LIBX264:
		set_codec_options(&options, LIBX264);
		codec = avcodec_find_encoder_by_name("libx264");
		break;
	case LIBX265:
		set_codec_options(&options, LIBX265);
		codec = avcodec_find_encoder_by_name("libx265");
		break;
	default:
		codec = NULL;
	}

	if (!codec)
		pexit("encoder not found");

	avctx = avcodec_alloc_context3(codec);
	if (!avctx)
		pexit("avcodec_alloc_context3 failed");

	avctx->time_base	= dc->avctx->time_base;
	avctx->pix_fmt		= codec->pix_fmts[0]; //first supported pixel format
	avctx->width		= dc->avctx->width;
	avctx->height		= dc->avctx->height;

	if (avcodec_open2(avctx, avctx->codec, &options) < 0)
		pexit("avcodec_open2 failed");

	ec->frame_queue = dc->frame_queue;
	/* output queues have length 1 to enforce RT processing */
	ec->packet_queue = queue_init(1);
	ec->lag_queue = queue_init(1);

	ec->avctx = avctx;
	ec->options = options;
	ec->w_ctx = wc;
	ec->id = id;

	return ec;
}

void encoder_free(encoder_context **ec)
{
	encoder_context *e;

	e = *ec;
	queue_free(e->packet_queue);
	queue_free(e->lag_queue);
	avcodec_free_context(&e->avctx);
	av_dict_free(&e->options);
	free(e);
	*ec = NULL;
}

void supply_frame(AVCodecContext *avctx, AVFrame *frame)
{
	int ret;

	ret = avcodec_send_frame(avctx, frame);
	if (ret == AVERROR(EAGAIN))
		pexit("API break: encoder send and receive returns EAGAIN");
	else if (ret == AVERROR_EOF)
		pexit("Encoder has already been flushed");
	else if (ret == AVERROR(EINVAL))
		pexit("codec invalid, not open or requires flushing");
	else if (ret == AVERROR(ENOMEM))
		pexit("memory allocation failed");
}

int encoder_thread(void *ptr)
{
	encoder_context *ec = (encoder_context *) ptr;
	AVFrame *frame;
	AVPacket *packet;
	AVFrameSideData *sd;
	float *descr;
	size_t descr_size = 4*sizeof(float);
	int ret;
	int64_t *timestamp;

	packet = av_packet_alloc(); //NULL check in loop.

	for (;;) {
		if (!packet)
			pexit("av_packet_alloc failed");

		ret = avcodec_receive_packet(ec->avctx, packet);
		if (ret == 0) {
			queue_append(ec->packet_queue, packet);
			packet = av_packet_alloc();
			continue;
		} else if (ret == AVERROR(EAGAIN)) {
			frame = queue_extract(ec->frame_queue);

			if (!frame)
				break;

			sd = av_frame_new_side_data(frame, AV_FRAME_DATA_FOVEATION_DESCRIPTOR, descr_size);
			if (!sd)
				pexit("side data allocation failed");
			descr = foveation_descriptor(ec->w_ctx);
			sd->data = (uint8_t *) descr;

			frame->pict_type = 0; //keep undefined to prevent warnings
			supply_frame(ec->avctx, frame);
			av_frame_free(&frame);

			timestamp = malloc(sizeof(int64_t));
			if (!timestamp)
				perror("malloc failed");
			*timestamp = av_gettime_relative();

			queue_append(ec->lag_queue, timestamp);

		} else if (ret == AVERROR_EOF) {
			break;
		} else if (ret == AVERROR(EINVAL)) {
			pexit("avcodec_receive_packet failed");
		}
	}

	queue_append(ec->packet_queue, NULL);
	avcodec_close(ec->avctx);
	avcodec_free_context(&ec->avctx);
	return 0;
}

float *foveation_descriptor(window_context *wc)
{
	float *f;
	int width, height;

	SDL_GetWindowSize(wc->window, &width, &height);

	f = malloc(4*sizeof(float));
	if (!f)
		pexit("malloc failed");

	#ifdef ET
	// eye-tracking
	f[0] =
	f[1] =
	f[2] =
	f[3] =

	#else
	// fake mouse motion dummy values
	SDL_GetMouseState(&wc->mouse_x, &wc->mouse_y);
	f[0] = (float) wc->mouse_x / width;
	f[1] = (float) wc->mouse_y / height;
	f[2] = 0.3;
	f[3] = 20;
	#endif
	return f;
}

decoder_context *source_decoder_init(reader_context *rc, int queue_capacity)
{
	AVCodecContext *avctx;
	decoder_context *dc;
	int ret;
	int index = rc->stream_index;
	AVStream *stream = rc->format_ctx->streams[index];
	AVCodec *codec;

	avctx = avcodec_alloc_context3(NULL);
	if (!avctx)
		pexit("avcodec_alloc_context3 failed");

	ret = avcodec_parameters_to_context(avctx, stream->codecpar);
	if (ret < 0)
		pexit("avcodec_parameters_to_context failed");

	avctx->time_base = stream->time_base;

	codec = avcodec_find_decoder(avctx->codec_id);
	if (!codec)
		pexit("avcodec_find_decoder failed");

	avctx->codec_id = codec->id;

	ret = avcodec_open2(avctx, codec, NULL);
	if (ret < 0)
		pexit("avcodec_open2 failed");

	dc = malloc(sizeof(decoder_context));
	if (!dc)
		pexit("malloc failed");

	dc->packet_queue = rc->packet_queue;
	dc->frame_queue = queue_init(queue_capacity);
	dc->avctx = avctx;

	return dc;
}

void supply_packet(AVCodecContext *avctx, AVPacket *packet)
{
	int ret;

	ret = avcodec_send_packet(avctx, packet);
	if (ret == AVERROR(EAGAIN))
		pexit("API break: decoder send and receive returns EAGAIN");
	else if (ret == AVERROR_EOF)
		pexit("Decoder has already been flushed");
	else if (ret == AVERROR(EINVAL))
		pexit("codec invalid, not open or requires flushing");
	else if (ret == AVERROR(ENOMEM))
		pexit("memory allocation failed");
}

int decoder_thread(void *ptr)
{
	int ret;
	decoder_context *dc = (decoder_context *) ptr;
	AVCodecContext *avctx = dc->avctx;
	AVFrame *frame;
	AVPacket *packet;

	frame = av_frame_alloc();
	for (;;) {
		if (!frame)
			pexit("av_frame_alloc failed");

		ret = avcodec_receive_frame(avctx, frame);
		if (ret == 0) {
			// valid frame - enqueue and allocate new buffer
			queue_append(dc->frame_queue, frame);
			frame = av_frame_alloc();
			continue;
		} else if (ret == AVERROR(EAGAIN)) {
			//provide another packet to the decoder
			packet = queue_extract(dc->packet_queue);
			supply_packet(avctx, packet);
			av_packet_free(&packet);
			continue;
		} else if (ret == AVERROR_EOF) {
			break;
		} else if (ret == AVERROR(EINVAL)) {
			//fatal
			pexit("avcodec_receive_frame failed");
		}
	//note continue/break pattern before adding functionality here
	}

	//enqueue flush packet in
	queue_append(dc->frame_queue, NULL);
	avcodec_close(avctx);
	return 0;
}

decoder_context *fov_decoder_init(encoder_context *ec)
{
	AVCodecContext *avctx;
	AVCodec *codec;
	decoder_context *dc;
	int ret;

	codec = avcodec_find_decoder(ec->avctx->codec->id);

	if (!codec)
		pexit("avcodec_find_decoder_by_name failed");

	avctx = avcodec_alloc_context3(codec);
	if (!avctx)
		pexit("avcodec_alloc_context3 failed");

	ret = avcodec_open2(avctx, codec, NULL);
	if (ret < 0)
		pexit("avcodec_open2 failed");

	dc = malloc(sizeof(decoder_context));
	if (!dc)
		pexit("malloc failed");

	dc->packet_queue = ec->packet_queue;
	dc->frame_queue = queue_init(1);
	dc->avctx = avctx;

	return dc;
}

void decoder_free(decoder_context **dc)
{
	decoder_context *d;

	d = *dc;
	avcodec_free_context(&d->avctx);
	queue_free(d->frame_queue);
	/* packet_queue is freed by reader_free! */
	free(d);
	*dc = NULL;
}
