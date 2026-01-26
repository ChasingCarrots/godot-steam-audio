#ifndef PARAMETERIZED_AUDIO_STREAM_H
#define PARAMETERIZED_AUDIO_STREAM_H

#include "godot_cpp/classes/audio_stream.hpp"
#include "godot_cpp/classes/audio_stream_playback.hpp"
#include "godot_cpp/templates/hash_map.hpp"
#include "godot_cpp/templates/local_vector.hpp"

class AudioStreamPlaybackParameterized;

class ParameterizedAudioStreamInput : public godot::Resource {
	GDCLASS(ParameterizedAudioStreamInput, Resource)

	static void _bind_methods();

	godot::StringName input_name;
	godot::Ref<godot::AudioStream> audio_stream;
public:
	godot::StringName GetInputName() const { return input_name; }
	void SetInputName(godot::StringName inputName) {
		input_name = inputName;
		set_name(input_name);
	}
	godot::Ref<godot::AudioStream> GetAudioStream() const { return audio_stream; }
	void SetAudioStream(godot::Ref<godot::AudioStream> audioStream) { audio_stream = audioStream; }

	// this is only for usage in C++ and at runtime, so we just have it as a public variable here
	godot::PackedVector2Array input_as_buffer;
	int num_buffer_refs = 0;
	void reference_buffer();
	void dereference_buffer();
};

class ParameterCondition : public godot::Resource {
	GDCLASS(ParameterCondition, Resource)

protected:
	static void _bind_methods();

public:
	virtual bool check(godot::StringName parameterName, float parameterValue) = 0;
};

class ParameterConditionComparison : public ParameterCondition {
	GDCLASS(ParameterConditionComparison, ParameterCondition)

	static void _bind_methods();
public:
	enum Comparison {
		EQ, LT, GT, LTE, GTE, NEQ
	};
private:
	godot::StringName parameter_name;
	Comparison comparison_type = EQ;
	float value = 0;

	void update_name() {
		set_name(godot::vformat("%s %s %f",
			parameter_name,
			comparison_type == EQ ? "==" :
			comparison_type == LT ? "<" :
			comparison_type == GT ? ">" :
			comparison_type == LTE ? "<=" :
			comparison_type == GTE ? ">=" :
			"!=",
			value));
	}
public:
	godot::StringName GetParameterName() const { return parameter_name; }
	void SetParameterName(godot::StringName parameterName) { parameter_name = parameterName; update_name(); }
	int GetComparisonType() const { return comparison_type; }
	void SetComparisonType(int comparisonType) { comparison_type = static_cast<Comparison>(comparisonType); update_name(); }
	float GetValue() const { return value; }
	void SetValue(float value) { this->value = value; update_name(); }
	bool check(godot::StringName parameterName, float parameterValue) override {
		if (parameterName == parameter_name) {
			switch (comparison_type) {
				case EQ: return godot::Math::is_equal_approx(parameterValue, value);
				case NEQ: return !godot::Math::is_equal_approx(parameterValue, value);
				case LT: return parameterValue < value;
				case GT: return parameterValue > value;
				case LTE: return parameterValue <= value;
				case GTE: return parameterValue >= value;
			}
		}
		return true;
	}
};
VARIANT_ENUM_CAST(ParameterConditionComparison::Comparison);


class ParameterConditionRange : public ParameterCondition {
	GDCLASS(ParameterConditionRange, ParameterCondition)

	static void _bind_methods();

	godot::StringName parameter_name;
	float min_value = 0;
	float max_value = 1;
	void update_name() {
		set_name(godot::vformat("%f <= %s >= %f", min_value, parameter_name, max_value));
	}
public:
	godot::StringName GetParameterName() const { return parameter_name; }
	void SetParameterName(godot::StringName parameterName) { parameter_name = parameterName; update_name(); }
	float GetMinValue() const { return min_value; }
	void SetMinValue(float minValue) { min_value = minValue; update_name(); }
	float GetMaxValue() const { return max_value; }
	void SetMaxValue(float maxValue) { max_value = maxValue; update_name(); }
	bool check(godot::StringName parameterName, float parameterValue) override {
		if (parameterName == parameter_name) {
			return parameterValue >= min_value && parameterValue <= max_value;
		}
		return true;
	}
};

class ParameterizedOutputRuntimeInstanceBase {
public:
	virtual ~ParameterizedOutputRuntimeInstanceBase() = default;
	virtual void triggered() = 0;
	virtual bool mix_output_into_buffer(godot::AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames) =0;
};

class ParameterizedOutput : public godot::Resource {
	GDCLASS(ParameterizedOutput, Resource)
protected:
	static void _bind_methods();
	godot::StringName output_name;
	godot::TypedArray<ParameterCondition> conditions;
	godot::StringName triggered_by;

	void update_name() {
		set_name(godot::vformat("%s (%s)", output_name, triggered_by));
	}
public:
	godot::StringName GetOutputName() const { return output_name; }
	void SetOutputName(godot::StringName outputName) { output_name = outputName; update_name(); }
	godot::TypedArray<ParameterCondition> GetConditions() const { return conditions; }
	void SetConditions(godot::TypedArray<ParameterCondition> conditions) { this->conditions = conditions; }
	godot::StringName GetTriggeredBy() const { return triggered_by; }
	void SetTriggeredBy(godot::StringName triggeredBy) { triggered_by = triggeredBy; update_name(); }

	bool should_trigger(godot::StringName on_trigger, const godot::HashMap<godot::StringName, float>& parameters);

	virtual ParameterizedOutputRuntimeInstanceBase* create_runtime_instance(const AudioStreamPlaybackParameterized& from_playback) =0;
	virtual void release_runtime_instance(ParameterizedOutputRuntimeInstanceBase* instance) = 0;
};

class ParameterizedOutputRandomize : public ParameterizedOutput {
	GDCLASS(ParameterizedOutputRandomize, ParameterizedOutput)

	static void _bind_methods();

	float randomize_pitch = 1;
	float randomize_volume = 1;
	godot::TypedArray<godot::StringName> input_streams;
public:
	float GetRandomizePitch() const { return randomize_pitch; }
	void SetRandomizePitch(float randomizePitch) { randomize_pitch = randomizePitch; }
	float GetRandomizeVolume() const { return randomize_volume; }
	void SetRandomizeVolume(float randomizeVolume) { randomize_volume = randomizeVolume; }
	godot::TypedArray<godot::StringName> GetInputStreams() const { return input_streams; }
	void SetInputStreams(godot::TypedArray<godot::StringName> inputStreams) { input_streams = inputStreams; }

	ParameterizedOutputRuntimeInstanceBase *create_runtime_instance(const AudioStreamPlaybackParameterized &from_playback) override;
	void release_runtime_instance(ParameterizedOutputRuntimeInstanceBase *instance) override;
};

class ParameterizedOutputLooping : public ParameterizedOutput {
	GDCLASS(ParameterizedOutputLooping, ParameterizedOutput)

	static void _bind_methods();


	godot::StringName input_stream;
	godot::StringName modifying_parameter_name;
	float min_volume = 0;
	float max_volume = 0;
	float parameter_value_min_volume;
	float parameter_value_max_volume;
	float min_pitch = 1;
	float max_pitch = 1;
	float parameter_value_min_pitch;
	float parameter_value_max_pitch;
public:
	godot::StringName GetInputStream() const { return input_stream; }
	void SetInputStream(godot::StringName inputStream) { input_stream = inputStream; }
	godot::StringName GetModifyingParameterName() const { return modifying_parameter_name; }
	void SetModifyingParameterName(godot::StringName modifyingParameterName) { modifying_parameter_name = modifyingParameterName; }
	float GetMinVolume() const { return min_volume; }
	void SetMinVolume(float minVolume) { min_volume = minVolume; }
	float GetMaxVolume() const { return max_volume; }
	void SetMaxVolume(float maxVolume) { max_volume = maxVolume; }
	float GetParameterValueMinVolume() const { return parameter_value_min_volume; }
	void SetParameterValueMinVolume(float parameterValueMinVolume) { parameter_value_min_volume = parameterValueMinVolume; }
	float GetParameterValueMaxVolume() const { return parameter_value_max_volume; }
	void SetParameterValueMaxVolume(float parameterValueMaxVolume) { parameter_value_max_volume = parameterValueMaxVolume; }
	float GetMinPitch() const { return min_pitch; }
	void SetMinPitch(float minPitch) { min_pitch = minPitch; }
	float GetMaxPitch() const { return max_pitch; }
	void SetMaxPitch(float maxPitch) { max_pitch = maxPitch; }
	float GetParameterValueMinPitch() const { return parameter_value_min_pitch; }
	void SetParameterValueMinPitch(float parameterValueMinPitch) { parameter_value_min_pitch = parameterValueMinPitch; }
	float GetParameterValueMaxPitch() const { return parameter_value_max_pitch; }
	void SetParameterValueMaxPitch(float parameterValueMaxPitch) { parameter_value_max_pitch = parameterValueMaxPitch; }

	ParameterizedOutputRuntimeInstanceBase *create_runtime_instance(const AudioStreamPlaybackParameterized &from_playback) override;
	void release_runtime_instance(ParameterizedOutputRuntimeInstanceBase *instance) override;
};


class ParameterizedOutputGranularLinearSweep : public ParameterizedOutput {
	GDCLASS(ParameterizedOutputGranularLinearSweep, ParameterizedOutput)

	static void _bind_methods();

	godot::StringName input_stream;
	godot::StringName sweeping_parameter_name;
	float min_parameter_value;
	float max_parameter_value;
	float min_grain_size_milliseconds = 20;
	float max_grain_size_milliseconds = 20;
	float grain_jitter_percentage = 0.01f;
public:
	godot::StringName GetInputStream() const { return input_stream; }
	void SetInputStream(godot::StringName inputStream) { input_stream = inputStream; }
	godot::StringName GetSweepingParameterName() const { return sweeping_parameter_name; }
	void SetSweepingParameterName(godot::StringName sweepingParameterName) { sweeping_parameter_name = sweepingParameterName; }
	float GetMinParameterValue() const { return min_parameter_value; }
	void SetMinParameterValue(float minParameterValue) { min_parameter_value = minParameterValue; }
	float GetMaxParameterValue() const { return max_parameter_value; }
	void SetMaxParameterValue(float maxParameterValue) { max_parameter_value = maxParameterValue; }
	float GetMinGrainSizeMilliseconds() const { return min_grain_size_milliseconds; }
	void SetMinGrainSizeMilliseconds(float minGrainSizeMilliseconds) { min_grain_size_milliseconds = minGrainSizeMilliseconds; }
	float GetMaxGrainSizeMilliseconds() const { return max_grain_size_milliseconds; }
	void SetMaxGrainSizeMilliseconds(float maxGrainSizeMilliseconds) { max_grain_size_milliseconds = maxGrainSizeMilliseconds; }
	float GetGrainJitterPercentage() const { return grain_jitter_percentage; }
	void SetGrainJitterPercentage(float grainJitterPercentage) { grain_jitter_percentage = grainJitterPercentage; }

	ParameterizedOutputRuntimeInstanceBase *create_runtime_instance(const AudioStreamPlaybackParameterized &from_playback) override;
	void release_runtime_instance(ParameterizedOutputRuntimeInstanceBase *instance) override;
};


class AudioStreamParameterized : public godot::AudioStream {
	GDCLASS(AudioStreamParameterized, AudioStream)

	friend class AudioStreamPlaybackParameterized;

	static void _bind_methods();

	godot::TypedArray<godot::StringName> parameters;
	godot::TypedArray<godot::StringName> triggers;
	godot::TypedArray<ParameterizedAudioStreamInput> inputs;
	godot::TypedArray<ParameterizedOutput> outputs;

	int num_playbacks;

public:
	godot::Ref<godot::AudioStreamPlayback> _instantiate_playback() const override;
	godot::String _get_stream_name() const override;

	godot::TypedArray<godot::StringName> GetParameters() const { return parameters; }
	void SetParameters(godot::TypedArray<godot::StringName> parameters) { this->parameters = parameters; }
	godot::TypedArray<godot::StringName> GetTriggers() const { return triggers; }
	void SetTriggers(godot::TypedArray<godot::StringName> triggers) { this->triggers = triggers; }
	godot::TypedArray<ParameterizedAudioStreamInput> GetInputs() const { return inputs; }
	void SetInputs(godot::TypedArray<ParameterizedAudioStreamInput> inputs) { this->inputs = inputs; }
	godot::TypedArray<ParameterizedOutput> GetOutputs() const { return outputs; }
	void SetOutputs(godot::TypedArray<ParameterizedOutput> outputs) { this->outputs = outputs; }
};

class AudioStreamPlaybackParameterized : public godot::AudioStreamPlayback {
	GDCLASS(AudioStreamPlaybackParameterized, AudioStreamPlayback)

	static void _bind_methods();

	godot::Ref<AudioStreamParameterized> parent_stream;
	godot::HashMap<godot::StringName, float> parameters;
	godot::LocalVector<ParameterizedOutputRuntimeInstanceBase*> runtime_outputs;
public:
	~AudioStreamPlaybackParameterized() override;
	const AudioStreamParameterized& GetParent() const { return *parent_stream.ptr(); }
	const godot::HashMap<godot::StringName, float>& GetCurrentParameterValues() const { return parameters; }
	float GetCurrentParameterValue(godot::StringName name) const {
		const float* value_if_there = parameters.getptr(name);
		if (value_if_there == nullptr)
			return 0;
		return *value_if_there;
	}

	void initialize(godot::Ref<AudioStreamParameterized> parent);
	void set_parameter(godot::StringName parameter_name, float value);
	void trigger(godot::StringName trigger_name);

	void _start(double p_from_pos) override;
	void _stop() override;
	bool _is_playing() const override;
	int32_t _mix(godot::AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames) override;
};

#endif //PARAMETERIZED_AUDIO_STREAM_H
