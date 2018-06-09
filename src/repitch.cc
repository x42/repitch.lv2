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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>

#include <rubberband/RubberBandStretcher.h> 

typedef struct {
	LV2_URID atom_Blank;
	LV2_URID atom_Object;
	LV2_URID atom_Sequence;
	LV2_URID midi_MidiEvent;
	LV2_URID atom_Float;
	LV2_URID atom_Int;
	LV2_URID atom_Long;
	LV2_URID time_Position;
	LV2_URID time_bar;
	LV2_URID time_barBeat;
	LV2_URID time_beatUnit;
	LV2_URID time_beatsPerBar;
	LV2_URID time_beatsPerMinute;
	LV2_URID time_speed;
	LV2_URID time_frame;
} RePitchURIs;

typedef struct {
	/* ports */
	const LV2_Atom_Sequence* control;
	float* p_in;
	float* p_out;

	RePitchURIs uris;

	/* LV2 Output */
	LV2_Log_Log* log;
	LV2_Log_Logger logger;

	/* Host Time */
	bool     host_info;
	float    host_bpm;
	double   bar_beats;
	float    host_speed;
	int      host_div;
	int64_t  host_frame;

	/* Settings */
	double sample_rate;

	RubberBand::RubberBandStretcher* stretcher;
} RePitch;

/* *****************************************************************************
 * helper functions
 */

/** map uris */
static void
map_uris (LV2_URID_Map* map, RePitchURIs* uris)
{
	uris->atom_Blank          = map->map (map->handle, LV2_ATOM__Blank);
	uris->atom_Object         = map->map (map->handle, LV2_ATOM__Object);
	uris->midi_MidiEvent      = map->map (map->handle, LV2_MIDI__MidiEvent);
	uris->atom_Sequence       = map->map (map->handle, LV2_ATOM__Sequence);
	uris->time_Position       = map->map (map->handle, LV2_TIME__Position);
	uris->atom_Long           = map->map (map->handle, LV2_ATOM__Long);
	uris->atom_Int            = map->map (map->handle, LV2_ATOM__Int);
	uris->atom_Float          = map->map (map->handle, LV2_ATOM__Float);
	uris->time_bar            = map->map (map->handle, LV2_TIME__bar);
	uris->time_barBeat        = map->map (map->handle, LV2_TIME__barBeat);
	uris->time_beatUnit       = map->map (map->handle, LV2_TIME__beatUnit);
	uris->time_beatsPerBar    = map->map (map->handle, LV2_TIME__beatsPerBar);
	uris->time_beatsPerMinute = map->map (map->handle, LV2_TIME__beatsPerMinute);
	uris->time_speed          = map->map (map->handle, LV2_TIME__speed);
	uris->time_frame          = map->map (map->handle, LV2_TIME__frame);
}

/**
 * Update the current position based on a host message. This is called by
 * run() when a time:Position is received.
 */
static void
update_position (RePitch* self, const LV2_Atom_Object* obj)
{
	const RePitchURIs* uris = &self->uris;

	LV2_Atom* bar   = NULL;
	LV2_Atom* beat  = NULL;
	LV2_Atom* bunit = NULL;
	LV2_Atom* bpb   = NULL;
	LV2_Atom* bpm   = NULL;
	LV2_Atom* speed = NULL;
	LV2_Atom* frame = NULL;

	lv2_atom_object_get (
			obj,
			uris->time_bar, &bar,
			uris->time_barBeat, &beat,
			uris->time_beatUnit, &bunit,
			uris->time_beatsPerBar, &bpb,
			uris->time_beatsPerMinute, &bpm,
			uris->time_speed, &speed,
			uris->time_frame, &frame,
			NULL);

	if (   bpm   && bpm->type == uris->atom_Float
			&& bpb   && bpb->type == uris->atom_Float
			&& bar   && bar->type == uris->atom_Long
			&& beat  && beat->type == uris->atom_Float
			&& bunit && bunit->type == uris->atom_Int
			&& speed && speed->type == uris->atom_Float
			&& frame && frame->type == uris->atom_Long)
	{
		float    _bpb   = ((LV2_Atom_Float*)bpb)->body;
		int64_t  _bar   = ((LV2_Atom_Long*)bar)->body;
		float    _beat  = ((LV2_Atom_Float*)beat)->body;

		self->host_div   = ((LV2_Atom_Int*)bunit)->body;
		self->host_bpm   = ((LV2_Atom_Float*)bpm)->body;
		self->host_speed = ((LV2_Atom_Float*)speed)->body;
		self->host_frame = ((LV2_Atom_Long*)frame)->body;

		self->bar_beats  = _bar * _bpb + _beat * self->host_div / 4.0;
		self->host_info  = true;
		if (self->host_frame < 0) {
			self->host_info  = false;
		}
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
	RePitch* self = (RePitch*)calloc (1, sizeof (RePitch));
	LV2_URID_Map* map = NULL;

	int i;
	for (i=0; features[i]; ++i) {
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

	self->stretcher = new RubberBand::RubberBandStretcher (rate, 1, RubberBand::RubberBandStretcher::OptionProcessRealTime );
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

	self->stretcher->setPitchScale (1.0 / speed);

	//self->stretcher->getLatency ();
	self->stretcher->process (&self->p_in, n_samples, false);

	if (self->stretcher->available () < n_samples) {
		memset (self->p_out, 0, n_samples * sizeof (float));
	} else {
		self->stretcher->retrieve (&self->p_out, n_samples);
	}

	/* keep track of host position.. */
	if (self->host_info) {
		self->bar_beats += n_samples * self->host_bpm * self->host_speed / (60.0 * self->sample_rate);
		self->host_frame += n_samples * self->host_speed;
	}

}

static void
cleanup (LV2_Handle instance)
{
	RePitch* self = (RePitch*)instance;
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
	NULL,
	run,
	NULL,
	cleanup,
	extension_data
};

#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
#    define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
#    define LV2_SYMBOL_EXPORT  __attribute__ ((visibility ("default")))
#endif
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
