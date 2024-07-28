#include <StdInc.h>
#include <EntitySystem.h>

#include <Hooking.h>

static hook::cdecl_stub<fwEntity* (int handle)> getScriptEntity([]()
{
	return hook::pattern("45 8B C1 41 C1 F8 08 45 38 0C 00 75 ? 8B 42 ? 41 0F AF C0").count(1).get(0).get<void>(-81);
});

fwEntity* rage::fwScriptGuid::GetBaseFromGuid(int handle)
{
	return getScriptEntity(handle);
}

static hook::cdecl_stub<fwArchetype*(uint32_t nameHash, rage::fwModelId& id)> getArchetype([]()
{
	return hook::get_call(hook::pattern("8B 4E 08 C1 EB 05 80 E3 01 E8").count(1).get(0).get<void>(9));
});

fwArchetype* rage::fwArchetypeManager::GetArchetypeFromHashKey(uint32_t hash, fwModelId& id)
{
	return getArchetype(hash, id);
}

fwArchetype* rage::fwArchetypeManager::GetArchetypeFromHashKeySafe(uint32_t hash, fwModelId& id)
{
	__try
	{
		return getArchetype(hash, id);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static hook::cdecl_stub<void*(fwExtensionList*, uint32_t)> getExtension([]()
{
	return hook::get_pattern("83 FA 3E 73 17 8B C7 44 8B C7 48 C1 E8 05 41 83 E0 1F", -0x11);
});

static hook::cdecl_stub<void(fwExtensionList*, rage::fwExtension*)> addExtension([]()
{
	return hook::get_pattern("48 89 18 4C 8B 07 4C 89 40 08 48 89 07 48 8B 03 FF 10", -0x1D);
});

void fwExtensionList::Add(rage::fwExtension* extension)
{
	return addExtension(this, extension);
}

void* fwExtensionList::Get(uint32_t id)
{
	return getExtension(this, id);
}
