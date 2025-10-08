#include "parameterized_audio_stream.h"

#include "godot_cpp/classes/audio_server.hpp"
#include "godot_cpp/classes/random_number_generator.hpp"

#include "profiling.h"

using namespace godot;

// -------------------- ParameterizedAudioStreamInput --------------------
void ParameterizedAudioStreamInput::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_input_name"), &ParameterizedAudioStreamInput::GetInputName);
	ClassDB::bind_method(D_METHOD("set_input_name", "input_name"), &ParameterizedAudioStreamInput::SetInputName);
	ClassDB::bind_method(D_METHOD("get_audio_stream"), &ParameterizedAudioStreamInput::GetAudioStream);
	ClassDB::bind_method(D_METHOD("set_audio_stream", "audio_stream"), &ParameterizedAudioStreamInput::SetAudioStream);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "input_name"), "set_input_name", "get_input_name");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "audio_stream", PROPERTY_HINT_RESOURCE_TYPE, "AudioStream"), "set_audio_stream", "get_audio_stream");
}

void ParameterizedAudioStreamInput::reference_buffer() {
	num_buffer_refs += 1;
	if (!input_as_buffer.is_empty()) {
		return;
	}
	auto stream_playback = audio_stream->instantiate_playback();
	stream_playback->start(0);
	int num_samples = audio_stream->get_length() * AudioServer::get_singleton()->get_mix_rate();
	input_as_buffer = stream_playback->mix_audio(1, num_samples);
}

void ParameterizedAudioStreamInput::dereference_buffer() {
	num_buffer_refs -= 1;
	if (num_buffer_refs == 0) {
		input_as_buffer.clear();
		input_as_buffer = {};
	}
}

// -------------------- ParameterCondition --------------------
void ParameterCondition::_bind_methods() {

}

// -------------------- ParameterConditionComparison --------------------
void ParameterConditionComparison::_bind_methods() {
	BIND_ENUM_CONSTANT(EQ);
	BIND_ENUM_CONSTANT(LT);
	BIND_ENUM_CONSTANT(GT);
	BIND_ENUM_CONSTANT(LTE);
	BIND_ENUM_CONSTANT(GTE);
	BIND_ENUM_CONSTANT(NEQ);

	ClassDB::bind_method(D_METHOD("get_parameter_name"), &ParameterConditionComparison::GetParameterName);
	ClassDB::bind_method(D_METHOD("set_parameter_name", "parameter_name"), &ParameterConditionComparison::SetParameterName);
	ClassDB::bind_method(D_METHOD("get_comparison_type"), &ParameterConditionComparison::GetComparisonType);
	ClassDB::bind_method(D_METHOD("set_comparison_type", "comparison_type"), &ParameterConditionComparison::SetComparisonType);
	ClassDB::bind_method(D_METHOD("get_value"), &ParameterConditionComparison::GetValue);
	ClassDB::bind_method(D_METHOD("set_value", "value"), &ParameterConditionComparison::SetValue);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "parameter_name"), "set_parameter_name", "get_parameter_name");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "comparison_type", PROPERTY_HINT_ENUM, "EQ,LT,GT,LTE,GTE,NEQ"), "set_comparison_type", "get_comparison_type");

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "value"), "set_value", "get_value");
}

// -------------------- ParameterConditionRange --------------------
void ParameterConditionRange::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_parameter_name"), &ParameterConditionRange::GetParameterName);
	ClassDB::bind_method(D_METHOD("set_parameter_name", "parameter_name"), &ParameterConditionRange::SetParameterName);
	ClassDB::bind_method(D_METHOD("get_min_value"), &ParameterConditionRange::GetMinValue);
	ClassDB::bind_method(D_METHOD("set_min_value", "min_value"), &ParameterConditionRange::SetMinValue);
	ClassDB::bind_method(D_METHOD("get_max_value"), &ParameterConditionRange::GetMaxValue);
	ClassDB::bind_method(D_METHOD("set_max_value", "max_value"), &ParameterConditionRange::SetMaxValue);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "parameter_name"), "set_parameter_name", "get_parameter_name");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_value"), "set_min_value", "get_min_value");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_value"), "set_max_value", "get_max_value");
}

// -------------------- ParameterizedOutput --------------------
void ParameterizedOutput::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_conditions"), &ParameterizedOutput::GetConditions);
	ClassDB::bind_method(D_METHOD("set_conditions", "conditions"), &ParameterizedOutput::SetConditions);
	ClassDB::bind_method(D_METHOD("get_triggered_by"), &ParameterizedOutput::GetTriggeredBy);
	ClassDB::bind_method(D_METHOD("set_triggered_by", "triggered_by"), &ParameterizedOutput::SetTriggeredBy);
	ClassDB::bind_method(D_METHOD("get_output_name"), &ParameterizedOutput::GetOutputName);
	ClassDB::bind_method(D_METHOD("set_output_name", "output_name"), &ParameterizedOutput::SetOutputName);
	
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "output_name"), "set_output_name", "get_output_name");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "conditions", PROPERTY_HINT_ARRAY_TYPE, "ParameterCondition"), "set_conditions", "get_conditions");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "triggered_by"), "set_triggered_by", "get_triggered_by");
}

bool ParameterizedOutput::should_trigger(StringName on_trigger, const HashMap<StringName, float> &parameters) {
	PROFILE_FUNCTION();
	if (on_trigger != triggered_by)
		return false;
	for (int i = 0; i < conditions.size(); ++i) {
		const auto condition = cast_to<ParameterCondition>(conditions[i]);
		for (auto param : parameters) {
			if (!condition->check(param.key, param.value)) {
				return false;
			}
		}
	}
	return true;
}

// -------------------- ParameterizedOutputRandomize --------------------
void ParameterizedOutputRandomize::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_randomize_pitch"), &ParameterizedOutputRandomize::GetRandomizePitch);
	ClassDB::bind_method(D_METHOD("set_randomize_pitch", "randomize_pitch"), &ParameterizedOutputRandomize::SetRandomizePitch);
	ClassDB::bind_method(D_METHOD("get_randomize_volume"), &ParameterizedOutputRandomize::GetRandomizeVolume);
	ClassDB::bind_method(D_METHOD("set_randomize_volume", "randomize_volume"), &ParameterizedOutputRandomize::SetRandomizeVolume);
	ClassDB::bind_method(D_METHOD("get_input_streams"), &ParameterizedOutputRandomize::GetInputStreams);
	ClassDB::bind_method(D_METHOD("set_input_streams", "input_streams"), &ParameterizedOutputRandomize::SetInputStreams);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "randomize_pitch"), "set_randomize_pitch", "get_randomize_pitch");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "randomize_volume"), "set_randomize_volume", "get_randomize_volume");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "input_streams", PROPERTY_HINT_ARRAY_TYPE, "StringName"), "set_input_streams", "get_input_streams");
}

class ParameterizedOutputRandomizeRuntimeInstance : public ParameterizedOutputRuntimeInstanceBase {
public:
	float randomize_pitch;
	float randomize_volume;
	Ref<RandomNumberGenerator> randomizer;
	LocalVector<const ParameterizedAudioStreamInput*> inputs;
	int last_played_index = 0;

	struct RuntimeInputData {
		int input_index;
		int current_frame_number;
		float pitch;
		float volume;
	};
	LocalVector<RuntimeInputData> currently_playing_inputs;
	void triggered() override {
		if (inputs.is_empty())
			return;
		RuntimeInputData runtime_input {};
		runtime_input.input_index = randomizer->randi_range(0, inputs.size()-1);
		// super simple way to not play the same sound twice in a row:
		if (runtime_input.input_index == last_played_index && inputs.size() > 1) {
			runtime_input.input_index = (runtime_input.input_index + 1) % inputs.size();
		}
		last_played_index = runtime_input.input_index;
		runtime_input.pitch = randomizer->randf_range(-randomize_pitch, randomize_pitch);
		runtime_input.volume = randomizer->randf_range(-randomize_volume, randomize_volume);
		runtime_input.current_frame_number = 0;
		currently_playing_inputs.push_back(runtime_input);
	}

	bool mix_output_into_buffer(AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames) override {
		if (currently_playing_inputs.is_empty()) {
			return false;
		}
		PROFILE_FUNCTION();
		int current_input_index = 0;
		while (current_input_index < currently_playing_inputs.size()) {
			auto& cpi = currently_playing_inputs[current_input_index];
			const ParameterizedAudioStreamInput& input = *inputs[cpi.input_index];
			int num_frames_available = input.input_as_buffer.size() - cpi.current_frame_number;
			int num_frames_to_mix = Math::min(p_frames, num_frames_available);

			for (int i = 0; i < num_frames_to_mix; i++) {
				p_buffer[i].left += input.input_as_buffer[cpi.current_frame_number].x;
				p_buffer[i].right += input.input_as_buffer[cpi.current_frame_number].y;
				++cpi.current_frame_number;
			}

			if (num_frames_to_mix == num_frames_available) {
				currently_playing_inputs.remove_at(current_input_index);
			}
			else {
				++current_input_index;
			}
		}
		return true;
	}
};

ParameterizedOutputRuntimeInstanceBase *ParameterizedOutputRandomize::create_runtime_instance(const AudioStreamPlaybackParameterized &from_playback) {
	PROFILE_FUNCTION();
	auto* instance = new ParameterizedOutputRandomizeRuntimeInstance();
	instance->randomize_pitch = randomize_pitch;
	instance->randomize_volume = randomize_volume;
	for (int i = 0; i < input_streams.size(); ++i) {
		StringName input_stream_name = input_streams[i];
		for (int input_index=0; input_index < from_playback.GetParent().GetInputs().size(); ++input_index) {
			const auto input = cast_to<ParameterizedAudioStreamInput>(from_playback.GetParent().GetInputs()[input_index]);
			if (input->GetInputName() == input_stream_name) {
				instance->inputs.push_back(input);
				break;
			}
		}
	}
	static int random_seed = 54631;
	random_seed += 24462;
	instance->randomizer.instantiate();
	instance->randomizer->set_seed(random_seed);
	return instance;
}

void ParameterizedOutputRandomize::release_runtime_instance(ParameterizedOutputRuntimeInstanceBase *instance) {
	delete dynamic_cast<ParameterizedOutputRandomizeRuntimeInstance*>(instance);
}

// -------------------- AudioStreamParameterized --------------------
void ParameterizedOutputGranularLinearSweep::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_input_stream"), &ParameterizedOutputGranularLinearSweep::GetInputStream);
	ClassDB::bind_method(D_METHOD("set_input_stream", "input_stream"), &ParameterizedOutputGranularLinearSweep::SetInputStream);

	ClassDB::bind_method(D_METHOD("get_sweeping_parameter_name"), &ParameterizedOutputGranularLinearSweep::GetSweepingParameterName);
	ClassDB::bind_method(D_METHOD("set_sweeping_parameter_name", "sweeping_parameter_name"), &ParameterizedOutputGranularLinearSweep::SetSweepingParameterName);

	ClassDB::bind_method(D_METHOD("get_min_parameter_value"), &ParameterizedOutputGranularLinearSweep::GetMinParameterValue);
	ClassDB::bind_method(D_METHOD("set_min_parameter_value", "min_parameter_value"), &ParameterizedOutputGranularLinearSweep::SetMinParameterValue);

	ClassDB::bind_method(D_METHOD("get_max_parameter_value"), &ParameterizedOutputGranularLinearSweep::GetMaxParameterValue);
	ClassDB::bind_method(D_METHOD("set_max_parameter_value", "max_parameter_value"), &ParameterizedOutputGranularLinearSweep::SetMaxParameterValue);

	ClassDB::bind_method(D_METHOD("get_min_grain_size_milliseconds"), &ParameterizedOutputGranularLinearSweep::GetMinGrainSizeMilliseconds);
	ClassDB::bind_method(D_METHOD("set_min_grain_size_milliseconds", "min_grain_size_milliseconds"), &ParameterizedOutputGranularLinearSweep::SetMinGrainSizeMilliseconds);

	ClassDB::bind_method(D_METHOD("get_max_grain_size_milliseconds"), &ParameterizedOutputGranularLinearSweep::GetMaxGrainSizeMilliseconds);
	ClassDB::bind_method(D_METHOD("set_max_grain_size_milliseconds", "max_grain_size_milliseconds"), &ParameterizedOutputGranularLinearSweep::SetMaxGrainSizeMilliseconds);
	ClassDB::bind_method(D_METHOD("get_grain_jitter_percentage"), &ParameterizedOutputGranularLinearSweep::GetGrainJitterPercentage);
	ClassDB::bind_method(D_METHOD("set_grain_jitter_percentage", "grain_jitter_percent"), &ParameterizedOutputGranularLinearSweep::SetGrainJitterPercentage);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "input_stream"), "set_input_stream", "get_input_stream");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "sweeping_parameter_name"), "set_sweeping_parameter_name", "get_sweeping_parameter_name");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_parameter_value"), "set_min_parameter_value", "get_min_parameter_value");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_parameter_value"), "set_max_parameter_value", "get_max_parameter_value");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_grain_size_milliseconds"), "set_min_grain_size_milliseconds", "get_min_grain_size_milliseconds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_grain_size_milliseconds"), "set_max_grain_size_milliseconds", "get_max_grain_size_milliseconds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grain_jitter_percentage"), "set_grain_jitter_percentage", "get_grain_jitter_percentage");
}

class ParameterizedOutputGranularLinearSweepRuntimeInstance : public ParameterizedOutputRuntimeInstanceBase {
public:
	godot::StringName sweeping_parameter_name;
	float min_parameter_value;
	float max_parameter_value;
	float min_grain_size_milliseconds = 20;
	float max_grain_size_milliseconds = 20;
	int mix_rate = 44100;
	float grain_jitter_percent = 0.01f;
	int grain_fade_window_samples = 50;
	Ref<RandomNumberGenerator> randomizer;
	ParameterizedAudioStreamInput* sweep_buffer_input = nullptr;
	const AudioStreamPlaybackParameterized* parent_playback = nullptr;

	float total_num_samples;
	float min_grain_samples;
	float max_grain_samples;
	float total_num_grains_in_input;
	float linear_step_grains;
	float current_jitter_offset = 0.0f;
	static constexpr int NUM_NO_REPEAT_GRAINS = 8;
	int last_played_grains[NUM_NO_REPEAT_GRAINS];
	inline bool is_repeat(int grain_number) {
		for (int i = 0; i < NUM_NO_REPEAT_GRAINS; i++) { if (grain_number == last_played_grains[i]) return true; }
		return false;
	}
	void initialize_grain_calculation() {
		for (int i = 0; i < NUM_NO_REPEAT_GRAINS; i++) { last_played_grains[i] = 0; }
		total_num_samples = sweep_buffer_input->input_as_buffer.size();
		min_grain_samples = Math::floor(min_grain_size_milliseconds * mix_rate / 1000.0f);
		max_grain_samples = Math::floor(max_grain_size_milliseconds * mix_rate / 1000.0f);
		total_num_grains_in_input = (2.0f * total_num_samples)
			/ (min_grain_samples + max_grain_samples);
		linear_step_grains = (max_grain_samples - min_grain_samples) / (total_num_grains_in_input - 1.0f);
	}
	int get_grain_number(float factor) {
		if (min_grain_samples == max_grain_samples) {
			return Math::floor(total_num_grains_in_input * factor);
		}
		float sample_index = factor * total_num_samples;
		float a = min_grain_samples - linear_step_grains / 2.0f;
		float grain_number = (-a + Math::sqrt(a*a + 2.0f * linear_step_grains * sample_index)) / linear_step_grains;
		return Math::floor(grain_number);
	}
	int get_grain_start_index(int grain_number) {
		float grain_float = static_cast<float>(grain_number);
		return Math::floor( (linear_step_grains / 2.0f) * grain_float * grain_float +
			(min_grain_samples - linear_step_grains / 2.0f) * grain_float );
	}
	int get_grain_size(int grain_number) {
		return min_grain_samples + static_cast<float>(grain_number) * linear_step_grains;
	}

	struct ActiveGrainData {
		int grain_number;
		int input_start;
		float current_frame_number;
		int number_of_samples;
		int elapsed_samples;
	};
	ActiveGrainData CurrentGrain;
	ActiveGrainData NextGrain;

	void triggered() override {
		// we just sweep forever, no triggering neccessary
	}

	bool mix_output_into_buffer(AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames) override {
		PROFILE_FUNCTION();
		for (int frame_index = 0; frame_index < p_frames; frame_index++) {
			float current_volume = 1.0f;
			float next_volume = 0.0f;
			int remaining_in_current = CurrentGrain.number_of_samples - CurrentGrain.elapsed_samples;
			if (remaining_in_current == grain_fade_window_samples) {
				// start up the next grain!
				NextGrain = spawn_new_grain();
			}
			if (remaining_in_current <= grain_fade_window_samples) {
				float factor_to_next = 1.0f - Math::inverse_lerp(0.0f, grain_fade_window_samples, remaining_in_current);
				current_volume = Math::sqrt(1.0f - factor_to_next);
				next_volume = Math::sqrt(factor_to_next);
			}

			int current_buffer_index = CurrentGrain.input_start + CurrentGrain.elapsed_samples;
			p_buffer[frame_index].left += sweep_buffer_input->input_as_buffer[current_buffer_index].x * current_volume;
			p_buffer[frame_index].right += sweep_buffer_input->input_as_buffer[current_buffer_index].y * current_volume;
			CurrentGrain.elapsed_samples ++;
			if (next_volume > 0.0f) {
				int next_buffer_index = NextGrain.input_start + NextGrain.elapsed_samples;
				p_buffer[frame_index].left += sweep_buffer_input->input_as_buffer[next_buffer_index].x * next_volume;
				p_buffer[frame_index].right += sweep_buffer_input->input_as_buffer[next_buffer_index].y * next_volume;
				NextGrain.elapsed_samples ++;
			}
			if (CurrentGrain.elapsed_samples >= CurrentGrain.number_of_samples) {
				// the fade to the next has been completed, let's swap!
				CurrentGrain = NextGrain;
			}
		}

		return true;
	}

	ActiveGrainData spawn_new_grain() {
		PROFILE_FUNCTION();
		float parameter_as_fraction = Math::inverse_lerp(min_parameter_value, max_parameter_value, parent_playback->GetCurrentParameterValue(sweeping_parameter_name));
		// we'll randomize the fraction a little, to get variance...
		current_jitter_offset += 0.1f * randomizer->randf_range(-grain_jitter_percent, grain_jitter_percent);
		current_jitter_offset = Math::clamp(current_jitter_offset, -grain_jitter_percent, grain_jitter_percent);
		parameter_as_fraction += current_jitter_offset;
		parameter_as_fraction = Math::clamp(parameter_as_fraction, 0.0f, 1.0f);

		int grain_number = get_grain_number(parameter_as_fraction);
		while (grain_number >= total_num_grains_in_input-1 || is_repeat(grain_number)) {
			if (grain_number >= total_num_grains_in_input-1) {
				grain_number -= 1;
			}
			else {
				grain_number += randomizer->randi_range(-2, 2);
				if (grain_number < 0) grain_number = 1;
			}
		}
		for (int i = 0; i < NUM_NO_REPEAT_GRAINS - 1; ++i) {
			last_played_grains[i] = last_played_grains[i + 1];
		}
		last_played_grains[NUM_NO_REPEAT_GRAINS - 1] = grain_number;
		int start_frame = get_grain_start_index(grain_number);
		int num_frames = get_grain_size(grain_number);

		ActiveGrainData d{};
		d.grain_number = grain_number;
		d.input_start = start_frame;
		d.current_frame_number = 0;
		d.number_of_samples = num_frames;
		d.elapsed_samples = 0;
		return d;
	}
};



ParameterizedOutputRuntimeInstanceBase *ParameterizedOutputGranularLinearSweep::create_runtime_instance(const AudioStreamPlaybackParameterized &from_playback) {
	PROFILE_FUNCTION();
	auto* instance = new ParameterizedOutputGranularLinearSweepRuntimeInstance();
	instance->parent_playback = &from_playback;
	instance->sweeping_parameter_name = sweeping_parameter_name;
	instance->min_parameter_value = min_parameter_value;
	instance->max_parameter_value = max_parameter_value;
	instance->min_grain_size_milliseconds = min_grain_size_milliseconds;
	instance->max_grain_size_milliseconds = max_grain_size_milliseconds;
	instance->grain_jitter_percent = grain_jitter_percentage;
	for (int input_index=0; input_index < from_playback.GetParent().GetInputs().size(); ++input_index) {
		const auto input = cast_to<ParameterizedAudioStreamInput>(from_playback.GetParent().GetInputs()[input_index]);
		if (input->GetInputName() == input_stream) {
			instance->sweep_buffer_input = input;
			break;
		}
	}
	if (instance->sweep_buffer_input == nullptr) {
		print_error("ParameterizedOutputGranularLinearSweep could not find input stream with name ", input_stream);
		delete instance;
		return nullptr;
	}
	static int random_seed = 74637;
	random_seed += 24462;
	instance->randomizer.instantiate();
	instance->randomizer->set_seed(random_seed);
	instance->initialize_grain_calculation();
	instance->CurrentGrain = instance->spawn_new_grain();


	return instance;
}

void ParameterizedOutputGranularLinearSweep::release_runtime_instance(ParameterizedOutputRuntimeInstanceBase *instance) {
	delete dynamic_cast<ParameterizedOutputGranularLinearSweepRuntimeInstance*>(instance);
}

// -------------------- AudioStreamParameterized --------------------
void AudioStreamParameterized::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_parameters"), &AudioStreamParameterized::GetParameters);
	ClassDB::bind_method(D_METHOD("set_parameters", "parameters"), &AudioStreamParameterized::SetParameters);
	ClassDB::bind_method(D_METHOD("get_triggers"), &AudioStreamParameterized::GetTriggers);
	ClassDB::bind_method(D_METHOD("set_triggers", "triggers"), &AudioStreamParameterized::SetTriggers);
	ClassDB::bind_method(D_METHOD("get_inputs"), &AudioStreamParameterized::GetInputs);
	ClassDB::bind_method(D_METHOD("set_inputs", "inputs"), &AudioStreamParameterized::SetInputs);
	ClassDB::bind_method(D_METHOD("get_outputs"), &AudioStreamParameterized::GetOutputs);
	ClassDB::bind_method(D_METHOD("set_outputs", "outputs"), &AudioStreamParameterized::SetOutputs);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "parameters", PROPERTY_HINT_ARRAY_TYPE, "StringName"), "set_parameters", "get_parameters");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "triggers", PROPERTY_HINT_ARRAY_TYPE, "StringName"), "set_triggers", "get_triggers");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "inputs", PROPERTY_HINT_ARRAY_TYPE, "ParameterizedAudioStreamInput"), "set_inputs", "get_inputs");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "outputs", PROPERTY_HINT_ARRAY_TYPE, "ParameterizedOutput"), "set_outputs", "get_outputs");
}

godot::Ref<godot::AudioStreamPlayback> AudioStreamParameterized::_instantiate_playback() const {
	godot::Ref<AudioStreamPlaybackParameterized> playback;
	playback.instantiate();
	playback->initialize({this});
	return playback;
}

godot::String AudioStreamParameterized::_get_stream_name() const {
	return "AudioStreamParameterized";
}

// -------------------- AudioStreamPlaybackParameterized --------------------
void AudioStreamPlaybackParameterized::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_parameter", "parameter_name", "parameter_value"), &AudioStreamPlaybackParameterized::set_parameter);
	ClassDB::bind_method(D_METHOD("trigger", "trigger_name"), &AudioStreamPlaybackParameterized::trigger);
}

AudioStreamPlaybackParameterized::~AudioStreamPlaybackParameterized() {
	if (parent_stream.is_valid()) {
		for (int i = 0; i < parent_stream->GetInputs().size(); ++i) {
			auto input = cast_to<ParameterizedAudioStreamInput>( parent_stream->GetInputs()[i] );
			if(input==nullptr)
				continue;
			input->dereference_buffer();
		}
	}
}

void AudioStreamPlaybackParameterized::initialize(godot::Ref<AudioStreamParameterized> parent) {
	PROFILE_FUNCTION();
	parent_stream = parent;
	for (int i = 0; i < parent->GetInputs().size(); ++i) {
		auto input = cast_to<ParameterizedAudioStreamInput>( parent->GetInputs()[i] );
		ERR_CONTINUE(input==nullptr);
		input->reference_buffer();
	}
	for (int i = 0; i < parent->GetParameters().size(); ++i) {
		parameters[parent->GetParameters()[i]] = 0;
	}
	for (int i = 0; i < parent->GetOutputs().size(); ++i) {
		const auto output = cast_to<ParameterizedOutput>(parent->GetOutputs()[i]);
		if (output == nullptr) {
			continue;
		}
		runtime_outputs.push_back(
			output->create_runtime_instance(*this));
	}
}

void AudioStreamPlaybackParameterized::set_parameter(godot::StringName parameter_name, float value) {
	parameters[parameter_name] = value;
}

void AudioStreamPlaybackParameterized::trigger(godot::StringName trigger_name) {
	for (int i = 0; i < parent_stream->GetOutputs().size(); ++i) {
		const auto output = cast_to<ParameterizedOutput>(parent_stream->GetOutputs()[i]);
		if (output->should_trigger(trigger_name, parameters)) {
			runtime_outputs[i]->triggered();
		}
	}
}

void AudioStreamPlaybackParameterized::_start(double p_from_pos) {

}

void AudioStreamPlaybackParameterized::_stop() {

}

bool AudioStreamPlaybackParameterized::_is_playing() const {
	// TODO: create a parameter "StopPlayingWhenNoOutputsActive" and go through all outputs checking
	// for now we just stay in the playing/mixing state forever...
	return true;
}

int32_t AudioStreamPlaybackParameterized::_mix(godot::AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames) {
	PROFILE_FUNCTION();
	for (int i = 0; i < p_frames; ++i) {
		p_buffer[i].left = 0;
		p_buffer[i].right = 0;
	}
	for (auto runtime_output : runtime_outputs) {
		runtime_output->mix_output_into_buffer(p_buffer, p_rate_scale, p_frames);
	}
	return p_frames;
}
