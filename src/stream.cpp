#include "stream.hpp"
#include "config.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/packed_vector2_array.hpp"
#include "server.hpp"
#include "steam_audio.hpp"
#include "profiling.h"
#include <phonon.h>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

SteamAudioStream::SteamAudioStream() {}
SteamAudioStream::~SteamAudioStream() {}

void SteamAudioStream::_bind_methods() {}

Ref<AudioStreamPlayback> SteamAudioStream::_instantiate_playback() const {
	Ref<SteamAudioStreamPlayback> playback;
	playback.instantiate();
	playback->set_stream(stream);
	playback->parent = parent;

	return playback;
}

void SteamAudioStream::set_stream(Ref<AudioStream> p_stream) { stream = p_stream; }
Ref<AudioStream> SteamAudioStream::get_stream() { return this->stream; }

// ----------------------------------------------------
// SteamAudioStreamPlayback

SteamAudioStreamPlayback::SteamAudioStreamPlayback() {}
SteamAudioStreamPlayback::~SteamAudioStreamPlayback() {}

IPLDirectEffectParams getDirectParams(GlobalSteamAudioState* gs,
									  LocalSteamAudioState* ls,
                                      IPLCoordinateSpace3 source,
                                      IPLCoordinateSpace3 listener)
{
	IPLSimulationOutputs outputs{};
	iplSourceGetOutputs(ls->src.simulationSource, IPL_SIMULATIONFLAGS_DIRECT, &outputs);

    outputs.direct.transmissionType = ls->cfg.transmission_type == 0
		? IPL_TRANSMISSIONTYPE_FREQINDEPENDENT
		: IPL_TRANSMISSIONTYPE_FREQDEPENDENT;

    outputs.direct.flags = static_cast<IPLDirectEffectFlags>(0);
    if (!ls->cfg.is_dist_attn_on)
    {
        outputs.direct.distanceAttenuation = 1.0f;
    }
    else
    {
    	outputs.direct.flags = static_cast<IPLDirectEffectFlags>(outputs.direct.flags | IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION);
        IPLDistanceAttenuationModel distanceAttenuationModel{};
        distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
    	distanceAttenuationModel.minDistance = ls->cfg.min_attn_dist;

        outputs.direct.distanceAttenuation = iplDistanceAttenuationCalculate(gs->ctx, source.origin, listener.origin, &distanceAttenuationModel);
    }

    if (!ls->cfg.is_air_absorption_on)
    {
        outputs.direct.airAbsorption[0] = 1.0f;
        outputs.direct.airAbsorption[1] = 1.0f;
        outputs.direct.airAbsorption[2] = 1.0f;
    }
    else
    {
    	outputs.direct.flags = static_cast<IPLDirectEffectFlags>(outputs.direct.flags | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
        IPLAirAbsorptionModel airAbsorptionModel{};
        airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;

        iplAirAbsorptionCalculate(gs->ctx, source.origin, listener.origin, &airAbsorptionModel, outputs.direct.airAbsorption);
    }

    if (!ls->cfg.is_directivity_on)
    {
        outputs.direct.directivity = 1.0f;
    }
    else
    {
        outputs.direct.flags = static_cast<IPLDirectEffectFlags>(outputs.direct.flags | IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY);
        IPLDirectivity directivity{};
        directivity.dipoleWeight = ls->cfg.directivity_dipole_weight;
        directivity.dipolePower = ls->cfg.directivity_dipole_power;

        outputs.direct.directivity = iplDirectivityCalculate(gs->ctx, source, listener.origin, &directivity);
    }

    if (!ls->cfg.is_occlusion_on)
    {
        outputs.direct.occlusion = 1.0f;
    }
    else
    {
        outputs.direct.flags = static_cast<IPLDirectEffectFlags>(outputs.direct.flags | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION);
    }

    if (!ls->cfg.is_transmission_on)
    {
        outputs.direct.transmission[0] = 1.0f;
        outputs.direct.transmission[1] = 1.0f;
        outputs.direct.transmission[2] = 1.0f;
    }
    else
    {
    	outputs.direct.flags = static_cast<IPLDirectEffectFlags>(outputs.direct.flags | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
    }

    return outputs.direct;
}

int32_t SteamAudioStreamPlayback::_mix(AudioFrame *buffer, double rate_scale, int32_t frames) {
	PROFILE_FUNCTION()
	if (parent == nullptr) {
		return frames;
	}

	if (stream_playback.is_null()) {
		return frames;
	}

	if (Engine::get_singleton()->is_editor_hint()) {
		return frames;
	}

	auto gs = SteamAudioServer::get_singleton()->get_global_state(false);
	if (gs == nullptr) {
		return frames;
	}

	SteamAudio::log(SteamAudio::log_debug, "mixing");

	LocalSteamAudioState *ls = parent->get_local_state();
	if (ls == nullptr) { // probably being destroyed
		return frames;
	}
	std::unique_lock lock(ls->mux);

	// Some extra checks because at this point parent may have been deleted
	if (parent == nullptr) {
		return frames;
	}
	ls = parent->get_local_state();
	if (ls == nullptr || !ls->src.player) {
		return frames;
	}
	auto sourceCoordinates = ipl_coords_from(ls->src.player->get_global_transform());
	auto listenerCoordinates = gs->listener_coords;
	auto sourcePosition = sourceCoordinates.origin;
	auto direction = iplCalculateRelativeDirection(gs->ctx, sourcePosition, listenerCoordinates.origin, listenerCoordinates.ahead, listenerCoordinates.up);

	PackedVector2Array mixed_frames = stream_playback->get_raw_audio(rate_scale, frames);
	frames = int(mixed_frames.size());

	for (int i = 0; i < frames; i++) {
		ls->bufs.in.data[0][i] = mixed_frames[i].x;
		ls->bufs.in.data[1][i] = mixed_frames[i].y;
	}

	IPLDirectEffectParams directParams = getDirectParams(gs, ls, sourceCoordinates, listenerCoordinates);

	iplDirectEffectApply(
			ls->fx.direct, &directParams,
			&ls->bufs.in, &ls->bufs.direct);

	if (ls->cfg.is_binaural_on) {
		PROFILE_FUNCTION_NAMED(apply_binaural)
		IPLBinauralEffectParams binauralParams{};
		binauralParams.direction = direction;
		binauralParams.interpolation = ls->hrtfInterpolation;
		binauralParams.spatialBlend = 1.0f;
		// TODO the steamaudio fmod plugin uses 2 hrtfs here, maybe we need that as well?
		binauralParams.hrtf = gs->hrtf;

		iplBinauralEffectApply(ls->fx.binaural, &binauralParams, &ls->bufs.direct, &ls->bufs.out);
	} else {
		PROFILE_FUNCTION_NAMED(apply_panning)
		iplAudioBufferDownmix(gs->ctx, &ls->bufs.direct, &ls->bufs.mono);

		IPLPanningEffectParams panningParams{};
		panningParams.direction = direction;

		iplPanningEffectApply(ls->fx.panning, &panningParams, &ls->bufs.mono, &ls->bufs.out);
	}

	if(ls->src.simulationSource && ls->cfg.is_reflection_on /* TODO: || pathing_on */) {
		IPLSimulationOutputs outputs;
		iplSourceGetOutputs(ls->src.simulationSource, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
		if (outputs.reflections.ir != nullptr) {
			PROFILE_FUNCTION_NAMED(apply_reflection)
			iplAudioBufferDownmix(gs->ctx, &ls->bufs.in, &ls->bufs.mono);
			outputs.reflections.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
			outputs.reflections.numChannels = ambisonic_channels_from(ls->cfg.ambisonics_order);
			outputs.reflections.irSize = int(SteamAudioConfig::max_refl_duration * float(gs->audio_cfg.samplingRate));
			outputs.reflections.tanDevice = nullptr;

			iplReflectionEffectApply(ls->fx.refl, &outputs.reflections, &ls->bufs.mono, &ls->bufs.refl, nullptr);
			SteamAudio::log(SteamAudio::log_debug, "mixing: mixing reflection and direct buffers");
			IPLAmbisonicsDecodeEffectParams ambisonicsParams;
			ambisonicsParams.order = ls->cfg.ambisonics_order;
			ambisonicsParams.hrtf = gs->hrtf;
			ambisonicsParams.orientation = listenerCoordinates;
			ambisonicsParams.binaural = IPL_TRUE;

			iplAmbisonicsDecodeEffectApply(ls->fx.ambisonics, &ambisonicsParams, &ls->bufs.refl, &ls->bufs.refl_out);

			iplAudioBufferMix(gs->ctx, &ls->bufs.refl_out, &ls->bufs.out);
		}

		// TODO: skipped the "PathingEffect" for now, but here would be the place.
		// (line 1420 in fmod/src/spatialize_effect.cpp)
	}


	{
		PROFILE_FUNCTION_NAMED(writing_out_buffer)
		for (int i = 0; i < frames; i++) {
			buffer[i].left = ls->bufs.out.data[0][i];
			buffer[i].right = ls->bufs.out.data[1][i];
		}
	}

	SteamAudio::log(SteamAudio::log_debug, "mixing: done");
	return frames;
}

void SteamAudioStreamPlayback::_bind_methods() {
	ClassDB::bind_method(D_METHOD("play_stream", "stream", "from_offset", "volume_db", "pitch_scale"), &SteamAudioStreamPlayback::play_stream, DEFVAL(0), DEFVAL(0), DEFVAL(1.0));
}

int SteamAudioStreamPlayback::play_stream(const Ref<AudioStream> &p_stream, float p_from_offset, float p_volume_db, float p_pitch_scale) {
	if (Engine::get_singleton()->is_editor_hint()) {
		return 0;
	}

	stream = p_stream;
	stream_playback = stream->instantiate_playback();
	stream_playback->start(p_from_offset);

	return 0;
}

void SteamAudioStreamPlayback::_start(double from_pos) {
	if (stream_playback == nullptr) {
		if (stream != nullptr) {
			is_active.store(true);
			play_stream(stream, float(from_pos), 0.0, 1.0); // FIXME: do not assume these params
		}
		return;
	} else if (stream_playback->is_playing()) {
		return;
	}
	stream_playback->start(from_pos);
	is_active.store(true);
}

void SteamAudioStreamPlayback::_stop() {
	is_active.store(false);
	if (stream_playback == nullptr || !stream_playback->is_playing()) {
		return;
	}
	stream_playback->stop();
}

bool SteamAudioStreamPlayback::_is_playing() const { return is_active; }
void SteamAudioStreamPlayback::set_stream(Ref<AudioStream> p_stream) { stream = p_stream; }
Ref<AudioStreamPlayback> SteamAudioStreamPlayback::get_stream_playback() { return this->stream_playback; }
