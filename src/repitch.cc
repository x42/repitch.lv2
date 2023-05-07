/* repitch
 *
 * Copyright (C) 2018 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define REPITCH_URI "http://gareus.org/oss/lv2/repitch"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LV2_1_18_6
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/core/lv2.h>
#include <lv2/log/logger.h>
#include <lv2/midi/midi.h>
#include "lv2/time/time.h"
#include <lv2/urid/urid.h>
#else
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#endif

#include <rubberband/RubberBandStretcher.h>

typedef struct {
	enum { length = 8192,
	       mask   = length - 1 };

	float* data;
	size_t write_pos, read_pos;
} RingBuffer;

static RingBuffer*
new_ring_buffer ()
{
	float* data = (float*)calloc (RingBuffer::length, sizeof (float));
	if (!data) {
		return NULL;
	}
	RingBuffer* sb = (RingBuffer*)malloc (sizeof (RingBuffer));
	if (!sb) {
		return NULL;
	}
	sb->data      = data;
	sb->write_pos = 0;
	sb->read_pos  = 0;
	return sb;
}

static void
delete_ring_buffer (RingBuffer* sb)
{
	free (sb->data);
	free (sb);
}

static void
reset_ring_buffer (RingBuffer* sb)
{
	memset (sb->data, 0, RingBuffer::length * sizeof (float));
	sb->read_pos  = 0;
	sb->write_pos = 0;
}

static void
put_to_ring_buffer (RingBuffer* sb, const float* data, size_t len)
{
	assert (len <= RingBuffer::length);

	const size_t c = RingBuffer::length - sb->write_pos;
	if (c >= len) {
		memcpy (sb->data + sb->write_pos, data, len * sizeof (float));
		sb->write_pos = (sb->write_pos + len) & RingBuffer::mask;
	} else {
		memcpy (sb->data + sb->write_pos, data, c * sizeof (float));
		memcpy (sb->data, data + c, (len - c) * sizeof (float));
		sb->write_pos = len - c;
	}
}

static void
get_from_ring_buffer (RingBuffer* sb, float* dst, size_t len)
{
	if (sb->write_pos == sb->read_pos) {
		memset (dst, 0, len * sizeof (float));
		return;
	}
	const uint32_t pos = sb->read_pos;
	if (pos < sb->write_pos && pos > sb->write_pos - len) {
		const uint32_t d = len - (sb->write_pos - pos);
		memset (dst, 0, d * sizeof (float));
		memcpy (dst + d, sb->data + pos, (len - d) * sizeof (float));
		sb->read_pos = (pos + len - d) & RingBuffer::mask;
		return;
	}
	if (pos + len > RingBuffer::length) {
		const size_t c = RingBuffer::length - pos;
		memcpy (dst, sb->data + pos, c * sizeof (float));
		memcpy (dst + c, sb->data, (len - c) * sizeof (float));
		sb->read_pos = (len - c);
	} else {
		memcpy (dst, sb->data + pos, len * sizeof (float));
		sb->read_pos = (pos + len) & RingBuffer::mask;
	}
}

typedef struct {
	LV2_URID atom_Blank;
	LV2_URID atom_Object;
	LV2_URID atom_Float;
	LV2_URID time_Position;
	LV2_URID time_speed;
	LV2_URID time_scale;
} RePitchURIs;

typedef struct {
	/* ports */
	const LV2_Atom_Sequence* control;
	float*                   p_in;
	float*                   p_out;

	RePitchURIs uris;

	/* LV2 Output */
	LV2_Log_Log*   log;
	LV2_Log_Logger logger;

	/* Host Time */
	float  host_bpm;
	double bar_beats;
	float  host_speed;
	int    host_div;

	/* Settings */
	double sample_rate;

	RingBuffer* ring_buffer;
	float*      retrieve_buffer;

	RubberBand::RubberBandStretcher* stretcher;
} RePitch;

/* *****************************************************************************
 * helper functions
 */

/** map uris */
static void
map_uris (LV2_URID_Map* map, RePitchURIs* uris)
{
	uris->atom_Blank    = map->map (map->handle, LV2_ATOM__Blank);
	uris->atom_Object   = map->map (map->handle, LV2_ATOM__Object);
	uris->time_Position = map->map (map->handle, LV2_TIME__Position);
	uris->atom_Float    = map->map (map->handle, LV2_ATOM__Float);
	uris->time_speed    = map->map (map->handle, LV2_TIME__speed);
	uris->time_scale    = map->map (map->handle, "http://ardour.org/lv2/time#scale"); // XXX
}

/**
 * Update the current position based on a host message. This is called by
 * run() when a time:Position is received.
 */
static void
update_position (RePitch* self, const LV2_Atom_Object* obj)
{
	const RePitchURIs* uris = &self->uris;

	LV2_Atom* speed = NULL;

	lv2_atom_object_get (
	    obj,
	    uris->time_scale, &speed,
	    NULL);

	if (speed && speed->type == uris->atom_Float) {
		self->host_speed = ((LV2_Atom_Float*)speed)->body;
		return;
	}

	lv2_atom_object_get (
	    obj,
	    uris->time_speed, &speed,
	    NULL);

	if (speed && speed->type == uris->atom_Float) {
		self->host_speed = ((LV2_Atom_Float*)speed)->body;
	}
}

/* *****************************************************************************
 * LV2 Plugin
 */

static LV2_Handle
instantiate (const LV2_Descriptor*     descriptor,
             double                    rate,
             const char*               bundle_path,
             const LV2_Feature* const* features)
{
	RePitch*      self = (RePitch*)calloc (1, sizeof (RePitch));
	LV2_URID_Map* map  = NULL;

	int i;
	for (i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_URID__map)) {
			map = (LV2_URID_Map*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_LOG__log)) {
			self->log = (LV2_Log_Log*)features[i]->data;
		}
	}

	lv2_log_logger_init (&self->logger, map, self->log);

	if (!map) {
		lv2_log_error (&self->logger, "RePitch.lv2 error: Host does not support urid:map\n");
		free (self);
		return NULL;
	}

	map_uris (map, &self->uris);

	self->sample_rate = rate;

	self->ring_buffer     = new_ring_buffer ();
	self->retrieve_buffer = (float*)malloc (RingBuffer::length * sizeof (float));

	self->stretcher = new RubberBand::RubberBandStretcher (rate, 1, RubberBand::RubberBandStretcher::OptionProcessRealTime);
	//self->stretcher->setDebugLevel(3);

	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance,
              uint32_t   port,
              void*      data)
{
	RePitch* self = (RePitch*)instance;

	switch (port) {
		case 0:
			self->control = (const LV2_Atom_Sequence*)data;
			break;
		case 1:
			self->p_in = (float*)data;
			break;
		case 2:
			self->p_out = (float*)data;
			break;
		default:
			break;
	}
}

static void
activate (LV2_Handle instance)
{
	RePitch* self = (RePitch*)instance;

	reset_ring_buffer (self->ring_buffer);
	memset (self->retrieve_buffer, 0, RingBuffer::length * sizeof (float));
}

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	RePitch* self = (RePitch*)instance;
	if (!self->control) {
		return;
	}

	/* process control events */
	LV2_Atom_Event* ev = lv2_atom_sequence_begin (&(self->control)->body);
	while (!lv2_atom_sequence_is_end (&(self->control)->body, (self->control)->atom.size, ev)) {
		if (ev->body.type == self->uris.atom_Blank || ev->body.type == self->uris.atom_Object) {
			const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
			if (obj->body.otype == self->uris.time_Position) {
				update_position (self, obj);
			}
		}
		ev = lv2_atom_sequence_next (ev);
	}

	double speed = fabsf (self->host_speed);
	if (speed == 0) {
		speed = 1;
	}

	if (speed >= 256. || speed <= 1. / 256.) {
		/* TODO: gracefully handle this, consider ZOH up/downsampling
		 * and use half the value for setPitchScale().
		 * Or click-free fade to silence.
		 * All hope is lost when vari-speeding using extreme speed
		 * factors anyway.
		 */
		memset (self->p_out, 0, n_samples * sizeof (float));
		return;
	}

	self->stretcher->setPitchScale (1.0 / speed);

	// TODO report latency, include ringbuffer offset
	//self->stretcher->getLatency ();

	uint32_t     processed = 0;
	const float* proc_ptr  = self->p_in;

	while (processed < n_samples) {
		uint32_t in_chunk_size = self->stretcher->getSamplesRequired ();
		uint32_t samples_left  = n_samples - processed;

		if (samples_left < in_chunk_size) {
			in_chunk_size = samples_left;
		}

		self->stretcher->process (&proc_ptr, in_chunk_size, 0);

		processed += in_chunk_size;
		proc_ptr += in_chunk_size;

		const uint32_t avail          = self->stretcher->available ();
		const uint32_t out_chunk_size = self->stretcher->retrieve (&self->retrieve_buffer, avail);
		put_to_ring_buffer (self->ring_buffer, self->retrieve_buffer, out_chunk_size);
	}

	get_from_ring_buffer (self->ring_buffer, self->p_out, n_samples);
}

static void
cleanup (LV2_Handle instance)
{
	RePitch* self = (RePitch*)instance;
	delete_ring_buffer (self->ring_buffer);
	free (self->retrieve_buffer);
	delete self->stretcher;
	free (instance);
}

static const void*
extension_data (const char* uri)
{
	return NULL;
}

static const LV2_Descriptor descriptor = {
	REPITCH_URI,
	instantiate,
	connect_port,
	activate,
	run,
	NULL,
	cleanup,
	extension_data
};

/* clang-format off */
#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
# define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
# define LV2_SYMBOL_EXPORT __attribute__ ((visibility ("default")))
#endif
/* clang-format on */
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	switch (index) {
		case 0:
			return &descriptor;
		default:
			return NULL;
	}
}
