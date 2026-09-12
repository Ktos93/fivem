/*
 * This file is part of the Cfx project - https://cfx.re/
 *
 * See LICENSE in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include <EntitySystem.h>
#include <Hooking.h>
#include <ScriptEngine.h>
#include "Hooking.Stubs.h"

#include <cmath>
#include <cstring>

// The engine light setters change the light group shared by every instance of a
// model, so per-entity color/intensity is kept aside and applied to a temporary
// copy while a light is converted into a draw request.
//
// The override lives in an fwExtension of the entity (see EntitySystem.h), so the game
// destroys it together with the entity: no side table, no entity handle validation and
// no entity destructor hook are needed.
//
// Runtime CLightComponent (0x180 bytes, allocated in sub_140DDE8FC); only the
// two overridden fields are named. The color is split into its channels: the engine
// keeps the alpha byte there and overrides must not touch it.
struct LightColor
{
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t alpha;
};

struct alignas(16) LightAttributes
{
	char pad[0x58];
	float intensity;  // +0x58
	LightColor color; // +0x5C
	char pad2[0x180 - 0x60];
};

static_assert(sizeof(LightAttributes) == 0x180);
static_assert(sizeof(LightColor) == 4);
static_assert(offsetof(LightAttributes, intensity) == 0x58);
static_assert(offsetof(LightAttributes, color) == 0x5C);

struct LightGroup
{
	LightAttributes** lights;
	uint16_t count;
};

// Shared model light group getter (sub_1404D1024), called unchanged.
static hook::cdecl_stub<LightGroup*(fwArchetype*)> getLightGroupByArchetype([]()
{
	return hook::get_pattern("48 83 EC ? 48 83 64 24 ? ? 83 B9");
});

// Above the range the engine tracks in fwExtensionList's identifier bits (<62).
static constexpr int kLightOverrideExtensionId = 97;

class CLightOverride : public rage::fwExtension
{
public:
	static int GetClassId()
	{
		return kLightOverrideExtensionId;
	}

	int GetExtensionId() const override
	{
		return GetClassId();
	}

	void SetIntensity(float value)
	{
		intensity = value;
		hasIntensity = true;
	}

	void SetColor(uint8_t red, uint8_t green, uint8_t blue)
	{
		color.red = red;
		color.green = green;
		color.blue = blue;
		hasColor = true;
	}

	void Reset()
	{
		hasIntensity = false;
		hasColor = false;
	}

	bool IsActive() const
	{
		return hasIntensity || hasColor;
	}

	// Written from the script thread and read from the render path, which only takes a
	// snapshot of the two values, so a plain field is enough: a frame can at worst catch
	// the value from just before the write.
	bool hasIntensity = false;
	float intensity = 0.0f;
	bool hasColor = false;
	LightColor color{};
};

static LightGroup* GetLightGroup(fwEntity* entity)
{
	return entity && entity->GetArchetype() ? getLightGroupByArchetype(entity->GetArchetype()) : nullptr;
}

// Applies the override onto a pair of light values, shared by the draw path (which
// copies the whole component) and the getters (which need only these two fields).
static void ApplyOverride(float& intensity, LightColor& color, const CLightOverride& settings)
{
	if (settings.hasIntensity)
	{
		intensity = settings.intensity;
	}
	if (settings.hasColor)
	{
		// only the channels an override owns, the engine's alpha byte stays as it was
		color.red = settings.color.red;
		color.green = settings.color.green;
		color.blue = settings.color.blue;
	}
}

// The converters below copy the attributes into the draw request synchronously,
// so a temporary copy is enough and the shared engine component stays untouched.
static const LightAttributes* GetDrawAttributes(void* lightEntity, const LightAttributes* source, LightAttributes& overrideCopy)
{
	// CLightEntity::m_physical, assigned by sub_1405DD758.
	auto parent = reinterpret_cast<hook::FlexStruct*>(lightEntity)->At<fwEntity*>(0xE0);
	if (!parent)
	{
		return source;
	}

	auto settings = parent->GetExtension<CLightOverride>();
	if (!settings || !settings->IsActive())
	{
		return source;
	}

	std::memcpy(&overrideCopy, source, sizeof(overrideCopy));
	ApplyOverride(overrideCopy.intensity, overrideCopy.color, *settings);
	return &overrideCopy;
}

// sub_14061A2F0: Win64 argument 4 and the return value are floats (XMM3/XMM0).
static float (*g_prepareLight)(void*, void*, void*, float, uint32_t, uint32_t*, uint32_t, const void*, const void*, const LightAttributes*);
static float PrepareLight(void* lightEntity, void* request, void* matrix, float fade, uint32_t interior, uint32_t* entityFlags, uint32_t flags, const void* color1, const void* color2, const LightAttributes* attributes)
{
	LightAttributes overrideCopy;
	return g_prepareLight(lightEntity, request, matrix, fade, interior, entityFlags, flags, color1, color2, GetDrawAttributes(lightEntity, attributes, overrideCopy));
}

// sub_14061B128: the alternate render path also reads intensity from +0x58.
static float (*g_prepareVolumeLight)(void*, void*, void*, float, uint32_t, uint32_t, uint32_t, const LightAttributes*);
static float PrepareVolumeLight(void* lightEntity, void* request, void* matrix, float fade, uint32_t interior, uint32_t interiorFlags, uint32_t flags, const LightAttributes* attributes)
{
	LightAttributes overrideCopy;
	return g_prepareVolumeLight(lightEntity, request, matrix, fade, interior, interiorFlags, flags, GetDrawAttributes(lightEntity, attributes, overrideCopy));
}

// sub_1405C8934 separately reads the source color when preparing a corona.
static void (*g_drawCorona)(void*, void*, const LightAttributes*);
static void DrawCorona(void* lightEntity, void* request, const LightAttributes* attributes)
{
	LightAttributes overrideCopy;
	g_drawCorona(lightEntity, request, GetDrawAttributes(lightEntity, attributes, overrideCopy));
}

// Override of the entity, created on first use.
static CLightOverride* GetOverride(fwEntity* entity)
{
	auto settings = entity->GetExtension<CLightOverride>();
	if (!settings)
	{
		settings = new CLightOverride();
		entity->AddExtension(settings);
	}

	return settings;
}

struct LightValues
{
	float intensity = 0.0f;
	LightColor color{};
};

// Values one light of the entity draws with: the model's own values with the override
// applied, or zeroes when the entity has no such light.
static LightValues ReadLight(int handle, int index)
{
	LightValues values;

	auto entity = rage::fwScriptGuid::GetBaseFromGuid(handle);
	auto group = GetLightGroup(entity);
	if (!group || !group->lights || index < 0 || index >= group->count || !group->lights[index])
	{
		return values;
	}

	values.intensity = group->lights[index]->intensity;
	values.color = group->lights[index]->color;

	if (auto settings = entity->GetExtension<CLightOverride>())
	{
		ApplyOverride(values.intensity, values.color, *settings);
	}

	return values;
}

static HookFunction hookFunction([]()
{
	// Resolve every call site before patching any of them.
	auto lightCall = hook::get_pattern("E8 ? ? ? ? F3 0F 10 8D ? ? ? ? 0F 28 F0 41 0F 2F F0");
	auto volumeCall = hook::get_pattern("E8 ? ? ? ? 38 9D ? ? ? ? 0F 84 ? ? ? ? 4C 8B C6");
	// The bytes after the second corona call differ between game builds, so this
	// site is anchored on the branch in the same block, 22 bytes before the call.
	void* coronaCalls[] = {
		hook::get_pattern("38 9D ? ? ? ? 0F 84 ? ? ? ? 4C 8B C6 48 8D 55 ? 48 8B CF E8", 22),
		hook::get_pattern("E8 ? ? ? ? 48 8B CF E8 ? ? ? ? 83 7C 24")
	};
	hook::set_call(&g_prepareLight, lightCall);
	hook::set_call(&g_prepareVolumeLight, volumeCall);
	hook::set_call(&g_drawCorona, coronaCalls[0]);
	hook::call(lightCall, PrepareLight);
	hook::call(volumeCall, PrepareVolumeLight);
	for (auto call : coronaCalls)
	{
		hook::call(call, DrawCorona);
	}

	fx::ScriptEngine::RegisterNativeHandler("SET_ENTITY_LIGHT_INTENSITY", [](fx::ScriptContext& context)
	{
		int handle = context.GetArgument<int>(0);
		auto entity = rage::fwScriptGuid::GetBaseFromGuid(handle);
		auto group = GetLightGroup(entity);
		float intensity = context.GetArgument<float>(1);

		if (group && std::isfinite(intensity) && intensity >= 0.0f)
		{
			GetOverride(entity)->SetIntensity(intensity);
		}

		int count = group ? group->count : 0;

		context.SetResult<int>(count);
	});

	fx::ScriptEngine::RegisterNativeHandler("SET_ENTITY_LIGHT_COLOR", [](fx::ScriptContext& context)
	{
		int handle = context.GetArgument<int>(0);
		auto entity = rage::fwScriptGuid::GetBaseFromGuid(handle);
		auto group = GetLightGroup(entity);
		auto red = context.GetArgument<uint8_t>(1);
		auto green = context.GetArgument<uint8_t>(2);
		auto blue = context.GetArgument<uint8_t>(3);

		if (group && red >= 0 && red <= 255 && green >= 0 && green <= 255 && blue >= 0 && blue <= 255)
		{
			GetOverride(entity)->SetColor(red, green, blue);
		}

		int count = group ? group->count : 0;

		context.SetResult<int>(count);
	});

	fx::ScriptEngine::RegisterNativeHandler("GET_ENTITY_LIGHT_INTENSITY", [](fx::ScriptContext& context)
	{
		int handle = context.GetArgument<int>(0);
		int index = context.GetArgument<int>(1);
		float intensity = ReadLight(handle, index).intensity;

		context.SetResult<float>(intensity);
	});

	fx::ScriptEngine::RegisterNativeHandler("GET_ENTITY_LIGHT_COLOR", [](fx::ScriptContext& context)
	{
		int handle = context.GetArgument<int>(0);
		int index = context.GetArgument<int>(1);
		auto color = ReadLight(handle, index).color;

		// Red, green and blue output pointers; null is allowed.
		if (auto red = context.GetArgument<int*>(2))
		{
			*red = color.red;
		}
		if (auto green = context.GetArgument<int*>(3))
		{
			*green = color.green;
		}
		if (auto blue = context.GetArgument<int*>(4))
		{
			*blue = color.blue;
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("GET_ENTITY_LIGHT_COUNT", [](fx::ScriptContext& context)
	{
		int handle = context.GetArgument<int>(0);
		auto entity = rage::fwScriptGuid::GetBaseFromGuid(handle);
		auto group = GetLightGroup(entity);
		int count = group && group->lights ? group->count : 0;

		context.SetResult<int>(count);
	});

	fx::ScriptEngine::RegisterNativeHandler("RESET_ENTITY_LIGHTS", [](fx::ScriptContext& context)
	{
		int handle = context.GetArgument<int>(0);
		auto entity = rage::fwScriptGuid::GetBaseFromGuid(handle);

		if (auto settings = entity ? entity->GetExtension<CLightOverride>() : nullptr)
		{
			settings->Reset();
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("HAS_ENTITY_CUSTOM_LIGHTS", [](fx::ScriptContext& context)
	{
		int handle = context.GetArgument<int>(0);
		auto entity = rage::fwScriptGuid::GetBaseFromGuid(handle);
		auto settings = entity ? entity->GetExtension<CLightOverride>() : nullptr;
		bool hasCustomLights = settings && settings->IsActive();

		context.SetResult<bool>(hasCustomLights);
	});
});
