/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "TwolameEncoderPlugin.hxx"
#include "EncoderAPI.hxx"
#include "AudioFormat.hxx"

#include <twolame.h>

#include <glib.h>

#include <assert.h>
#include <string.h>

struct TwolameEncoder final {
	Encoder encoder;

	AudioFormat audio_format;
	float quality;
	int bitrate;

	twolame_options *options;

	unsigned char output_buffer[32768];
	size_t output_buffer_length;
	size_t output_buffer_position;

	/**
	 * Call libtwolame's flush function when the output_buffer is
	 * empty?
	 */
	bool flush;

	TwolameEncoder():encoder(twolame_encoder_plugin) {}

	bool Configure(const config_param &param, GError **error);
};

static inline GQuark
twolame_encoder_quark(void)
{
	return g_quark_from_static_string("twolame_encoder");
}

bool
TwolameEncoder::Configure(const config_param &param, GError **error)
{
	const char *value;
	char *endptr;

	value = param.GetBlockValue("quality");
	if (value != nullptr) {
		/* a quality was configured (VBR) */

		quality = g_ascii_strtod(value, &endptr);

		if (*endptr != '\0' || quality < -1.0 || quality > 10.0) {
			g_set_error(error, twolame_encoder_quark(), 0,
				    "quality \"%s\" is not a number in the "
				    "range -1 to 10, line %i",
				    value, param.line);
			return false;
		}

		if (param.GetBlockValue("bitrate") != nullptr) {
			g_set_error(error, twolame_encoder_quark(), 0,
				    "quality and bitrate are "
				    "both defined (line %i)",
				    param.line);
			return false;
		}
	} else {
		/* a bit rate was configured */

		value = param.GetBlockValue("bitrate");
		if (value == nullptr) {
			g_set_error(error, twolame_encoder_quark(), 0,
				    "neither bitrate nor quality defined "
				    "at line %i",
				    param.line);
			return false;
		}

		quality = -2.0;
		bitrate = g_ascii_strtoll(value, &endptr, 10);

		if (*endptr != '\0' || bitrate <= 0) {
			g_set_error(error, twolame_encoder_quark(), 0,
				    "bitrate at line %i should be a positive integer",
				    param.line);
			return false;
		}
	}

	return true;
}

static Encoder *
twolame_encoder_init(const config_param &param, GError **error_r)
{
	g_debug("libtwolame version %s", get_twolame_version());

	TwolameEncoder *encoder = new TwolameEncoder();

	/* load configuration from "param" */
	if (!encoder->Configure(param, error_r)) {
		/* configuration has failed, roll back and return error */
		delete encoder;
		return nullptr;
	}

	return &encoder->encoder;
}

static void
twolame_encoder_finish(Encoder *_encoder)
{
	TwolameEncoder *encoder = (TwolameEncoder *)_encoder;

	/* the real libtwolame cleanup was already performed by
	   twolame_encoder_close(), so no real work here */
	delete encoder;
}

static bool
twolame_encoder_setup(TwolameEncoder *encoder, GError **error)
{
	if (encoder->quality >= -1.0) {
		/* a quality was configured (VBR) */

		if (0 != twolame_set_VBR(encoder->options, true)) {
			g_set_error(error, twolame_encoder_quark(), 0,
				    "error setting twolame VBR mode");
			return false;
		}
		if (0 != twolame_set_VBR_q(encoder->options, encoder->quality)) {
			g_set_error(error, twolame_encoder_quark(), 0,
				    "error setting twolame VBR quality");
			return false;
		}
	} else {
		/* a bit rate was configured */

		if (0 != twolame_set_brate(encoder->options, encoder->bitrate)) {
			g_set_error(error, twolame_encoder_quark(), 0,
				    "error setting twolame bitrate");
			return false;
		}
	}

	if (0 != twolame_set_num_channels(encoder->options,
					  encoder->audio_format.channels)) {
		g_set_error(error, twolame_encoder_quark(), 0,
			    "error setting twolame num channels");
		return false;
	}

	if (0 != twolame_set_in_samplerate(encoder->options,
					   encoder->audio_format.sample_rate)) {
		g_set_error(error, twolame_encoder_quark(), 0,
			    "error setting twolame sample rate");
		return false;
	}

	if (0 > twolame_init_params(encoder->options)) {
		g_set_error(error, twolame_encoder_quark(), 0,
			    "error initializing twolame params");
		return false;
	}

	return true;
}

static bool
twolame_encoder_open(Encoder *_encoder, AudioFormat &audio_format,
		     GError **error)
{
	TwolameEncoder *encoder = (TwolameEncoder *)_encoder;

	audio_format.format = SampleFormat::S16;
	audio_format.channels = 2;

	encoder->audio_format = audio_format;

	encoder->options = twolame_init();
	if (encoder->options == nullptr) {
		g_set_error(error, twolame_encoder_quark(), 0,
			    "twolame_init() failed");
		return false;
	}

	if (!twolame_encoder_setup(encoder, error)) {
		twolame_close(&encoder->options);
		return false;
	}

	encoder->output_buffer_length = 0;
	encoder->output_buffer_position = 0;
	encoder->flush = false;

	return true;
}

static void
twolame_encoder_close(Encoder *_encoder)
{
	TwolameEncoder *encoder = (TwolameEncoder *)_encoder;

	twolame_close(&encoder->options);
}

static bool
twolame_encoder_flush(Encoder *_encoder, gcc_unused GError **error)
{
	TwolameEncoder *encoder = (TwolameEncoder *)_encoder;

	encoder->flush = true;
	return true;
}

static bool
twolame_encoder_write(Encoder *_encoder,
		      const void *data, size_t length,
		      gcc_unused GError **error)
{
	TwolameEncoder *encoder = (TwolameEncoder *)_encoder;
	const int16_t *src = (const int16_t*)data;

	assert(encoder->output_buffer_position ==
	       encoder->output_buffer_length);

	const unsigned num_frames =
		length / encoder->audio_format.GetFrameSize();

	int bytes_out = twolame_encode_buffer_interleaved(encoder->options,
							  src, num_frames,
							  encoder->output_buffer,
							  sizeof(encoder->output_buffer));
	if (bytes_out < 0) {
		g_set_error(error, twolame_encoder_quark(), 0,
			    "twolame encoder failed");
		return false;
	}

	encoder->output_buffer_length = (size_t)bytes_out;
	encoder->output_buffer_position = 0;
	return true;
}

static size_t
twolame_encoder_read(Encoder *_encoder, void *dest, size_t length)
{
	TwolameEncoder *encoder = (TwolameEncoder *)_encoder;

	assert(encoder->output_buffer_position <=
	       encoder->output_buffer_length);

	if (encoder->output_buffer_position == encoder->output_buffer_length &&
	    encoder->flush) {
		int ret = twolame_encode_flush(encoder->options,
					       encoder->output_buffer,
					       sizeof(encoder->output_buffer));
		if (ret > 0) {
			encoder->output_buffer_length = (size_t)ret;
			encoder->output_buffer_position = 0;
		}

		encoder->flush = false;
	}


	const size_t remainning = encoder->output_buffer_length
		- encoder->output_buffer_position;
	if (length > remainning)
		length = remainning;

	memcpy(dest, encoder->output_buffer + encoder->output_buffer_position,
	       length);

	encoder->output_buffer_position += length;

	return length;
}

static const char *
twolame_encoder_get_mime_type(gcc_unused Encoder *_encoder)
{
	return "audio/mpeg";
}

const EncoderPlugin twolame_encoder_plugin = {
	"twolame",
	twolame_encoder_init,
	twolame_encoder_finish,
	twolame_encoder_open,
	twolame_encoder_close,
	twolame_encoder_flush,
	twolame_encoder_flush,
	nullptr,
	nullptr,
	twolame_encoder_write,
	twolame_encoder_read,
	twolame_encoder_get_mime_type,
};
