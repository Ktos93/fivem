#include <StdInc.h>
#include <Hooking.h>
#include <Hooking.Patterns.h>
#include <Pool.h>
#include <GameInit.h>

#include <boost/algorithm/string.hpp>

//
// There is a global bitset used to store the enabled/disabled state of every AMV. This bitset has a hardcoded size
// of 6016 entries, once there are more AMVs than the bitset size, it starts reading/writing out-of-bounds. This causes
// the AMVs to start blinking and can potentially crash.
// 
// Here we increase the size of that bitset to fit the capacity of all the AMV pools.
//

static HookFunction hookFunction([]()
{
	// keep in sync with gameconfig
	constexpr size_t CAmbientMaskVolume_PoolSize = 6686;
	constexpr size_t CAmbientMaskVolumeDoor_PoolSize = 1850;
	constexpr size_t CAmbientMaskVolumeEntity_PoolSize = 150;

	constexpr size_t TotalAMVs = CAmbientMaskVolume_PoolSize + CAmbientMaskVolumeDoor_PoolSize + CAmbientMaskVolumeEntity_PoolSize;

	// Due to how AMVPresentBuffer is compiled, the number of blocks needs to match the form (N*32+28). AMVPresentBuffer copies the bitset
	// from update thread to the render thread bitset. The copy loop is vectorized to copy 32 blocks per iteration, then has an epilogue to
	// copy the remaining 28 blocks, which works out for the original 188 blocks. When resizing the bitset we need to pad the number of blocks
	// so it doesn't read/write out-of-bounds.
	constexpr size_t BitsetNumBlocksNoPadding = (TotalAMVs + 31) / 32; // minimum number of 32-bit blocks needed by the AMVs
	constexpr size_t BitsetPresentCopyNumIterations = (BitsetNumBlocksNoPadding - 28 + 31) / 32; // smallest N such that (N*32+28) >= BitsetNumBlocksNoPadding
	constexpr size_t BitsetNumBlocks = BitsetPresentCopyNumIterations * 32 + 28;
	constexpr size_t BitsetNumBytes = BitsetNumBlocks * sizeof(uint32_t);

	// x2 because the bitset is double-buffered, one for render thread and another one for update thread
	uint32_t* amvEnabledBitsetReplacement =  reinterpret_cast<uint32_t*>(hook::AllocateStubMemory(BitsetNumBytes * 2));
	memset(amvEnabledBitsetReplacement, 0, BitsetNumBytes * 2);

	// AMVInit
	{
		auto location = hook::get_pattern<uint8_t>("41 B8 ? ? ? ? 44 89 25");
		hook::put<int32_t>(location + 2, BitsetNumBytes * 2);
		hook::put<int32_t>(location + 0x10, (intptr_t)amvEnabledBitsetReplacement - (intptr_t)location - 0x10 - 4);
	}

	// AMVIsEnabled
	{
		
		auto location = hook::get_pattern<uint8_t>("48 69 C8 ? ? ? ? 44 8B C2");
		hook::put<int32_t>(location + 3, BitsetNumBlocks);
		hook::put<int32_t>(location + 0xD, (intptr_t)amvEnabledBitsetReplacement - (intptr_t)location - 0xD - 4);
	}

	// AMVSetEnabled
	{
		auto location = hook::get_pattern<uint8_t>("4C 69 C0 ? ? ? ? 0F B7 C9");
		hook::put<int32_t>(location + 3, BitsetNumBytes);
		hook::put<int32_t>(location + 0xD, (intptr_t)amvEnabledBitsetReplacement - (intptr_t)location - 0xD - 4);
	}

	// AMVPresentBuffer
	{
		auto location = hook::get_pattern<uint8_t>("48 69 D0 ? ? ? ? 41 8B C0");
		hook::put<int32_t>(location + 3, BitsetNumBytes);
		hook::put<int32_t>(location + 0x14, BitsetNumBytes);
		hook::put<int32_t>(location + 0x19, BitsetPresentCopyNumIterations);
		hook::put<int32_t>(location + 0xD, (intptr_t)amvEnabledBitsetReplacement - (intptr_t)location - 0xD - 4);
	}
});

//
// The game collects the streamed ambient mask volume zones by probing "amv_zone_0", "amv_zone_1", ... and ends the
// enumeration at the first name that is not a valid streaming entry - with the shipped data that is amv_zone_183.
// Map creators could therefore only replace the shipped zone files, or fight over the same free numbers.
//
// The first name the map does not ship is where the game would stop, and that slot (and each following one) is handed
// over to a zone file a resource streams under an "amv_zone_" name of its own, which lets every map have its own
// zones instead of sharing the shipped numbering. A zone file that replaces a shipped one never gets here - it
// overrides that streaming entry instead of adding one, and the probe loads that override by itself.
//
// The file indexes arrive with the streaming registration, which can happen before the level is loaded, so zone files
// are collected by name and handed over when the probe runs out of shipped names.
//

// strIndex base the loader adds to a probed index, read out of the request path
static uint32_t* g_amvZoneBase;

// zone files streamed by resources, with the streaming index each of them resolved to
static std::vector<std::pair<std::string, uint32_t>> g_amvZoneFiles;

// state of the current enumeration
static size_t g_amvZoneFileCursor;
static uint32_t g_amvZoneFilesByName;
static uint32_t g_amvZoneFilesBySlot;

// the file name without directories, lowercased
static std::string GetAmvZoneFileName(const std::string& name)
{
	auto fileName = name.substr(name.find_last_of("/\\") + 1);

	boost::algorithm::to_lower(fileName);

	return fileName;
}

namespace streaming
{
	void DLL_EXPORT AddAmvZoneFileIndex(const std::string& name, uint32_t strIndex)
	{
		auto baseName = GetAmvZoneFileName(name);

		// the prefix marks the zone files, which is also what the log is for
		if (baseName.compare(0, 9, "amv_zone_") != 0)
		{
			return;
		}

		trace("amv_zone: registered %s (streaming index %u)\n", baseName.c_str(), strIndex);

		// only the zone YMT is handed over to the probe, its texture dictionary loads when something asks for it
		if (baseName.find(".ymt") == std::string::npos)
		{
			return;
		}

		for (const auto& file : g_amvZoneFiles)
		{
			if (file.second == strIndex)
			{
				trace("amv_zone: %s (streaming index %u) is already collected\n", baseName.c_str(), strIndex);
				return;
			}
		}

		g_amvZoneFiles.emplace_back(baseName, strIndex);
	}
}

static void ClearAmvZoneFiles()
{
	g_amvZoneFiles.clear();
	g_amvZoneFileCursor = 0;
}

static void StartAmvZoneEnumeration()
{
	g_amvZoneFileCursor = 0;
	g_amvZoneFilesByName = 0;
	g_amvZoneFilesBySlot = 0;
}

static void OnAmvZoneProbe(uint32_t slot, int32_t streamIndex)
{
	// every enumeration starts with amv_zone_0
	if (slot == 0)
	{
		StartAmvZoneEnumeration();
	}

	if (streamIndex >= 0)
	{
		++g_amvZoneFilesByName;
	}
}

static int32_t TakeAmvZoneFile(uint32_t slot)
{
	while (g_amvZoneFileCursor < g_amvZoneFiles.size())
	{
		const auto& file = g_amvZoneFiles[g_amvZoneFileCursor++];

		// a file outside the zone module cannot be requested by the probe
		if (file.second < *g_amvZoneBase)
		{
			trace("amv_zone: %s is not in the zone module, skipped\n", file.first.c_str());
			continue;
		}

		++g_amvZoneFilesBySlot;
		trace("amv_zone: %s -> probe slot #%u (streaming index %u)\n", file.first.c_str(), slot, file.second);

		return static_cast<int32_t>(file.second - *g_amvZoneBase);
	}

	return -1;
}

static void OnAmvZoneProbeFinished()
{
	trace("amv_zone: %u zone files by name, %u resource zone files in free slots\n", g_amvZoneFilesByName, g_amvZoneFilesBySlot);
}

static HookFunction amvZoneHookFunction([]()
{
	static struct : jitasm::Frontend
	{
		intptr_t requestObject;
		intptr_t loadRequested;
		intptr_t onProbe;
		intptr_t onTakeFile;
		intptr_t onFinished;

		void Init(intptr_t request, intptr_t finish)
		{
			this->requestObject = request;
			this->loadRequested = finish;
			this->onProbe = reinterpret_cast<intptr_t>(OnAmvZoneProbe);
			this->onTakeFile = reinterpret_cast<intptr_t>(TakeAmvZoneFile);
			this->onFinished = reinterpret_cast<intptr_t>(OnAmvZoneProbeFinished);
		}

		virtual void InternalMain() override
		{
			// report the probed zone and its streaming index to the log; ebx and r15d are non-volatile,
			// so they survive the call
			mov(ecx, r15d);
			sub(ecx, 1);
			mov(edx, ebx);
			mov(rax, onProbe);
			sub(rsp, 0x20);
			call(rax);
			add(rsp, 0x20);

			// ebx holds the streaming index of the probed zone name, or -1 when there is no such file
			cmp(ebx, (uint32_t)-1);
			jz("missing");

			mov(rax, requestObject);
			jmp(rax);

			L("missing");

			// the counter is already incremented for the next name, so the slot is r15d - 1
			mov(ecx, r15d);
			sub(ecx, 1);
			mov(rax, onTakeFile);
			sub(rsp, 0x20);
			call(rax);
			add(rsp, 0x20);

			test(eax, eax);
			js("done");

			mov(ebx, eax);
			mov(rax, requestObject);
			jmp(rax);

			L("done");

			mov(rax, onFinished);
			sub(rsp, 0x20);
			call(rax);
			add(rsp, 0x20);

			mov(rax, loadRequested);
			jmp(rax);
		}
	} zoneProbeStub;

	{
		// the loop exit: <cmp ebx,-1> <jnz request> <xor edx,edx>, the point the enumeration gives up on a probe
		auto location = hook::get_pattern<uint8_t>("83 FB ? 0F 85 ? ? ? ? 33 D2");

		// the request path the original conditional jump went to
		const auto requestPtr = reinterpret_cast<intptr_t>(location) + 9 + *reinterpret_cast<int32_t*>(location + 5);

		// it has to start with <mov edx, cs:dword_1457C4988>, the base the probe index is added to
		if (*reinterpret_cast<uint8_t*>(requestPtr) != 0x8B)
		{
			trace("amv_zone: unexpected zone loader code, zones from resources stay unavailable\n");
			return;
		}

		g_amvZoneBase = hook::get_address<uint32_t*>(requestPtr + 2);

		zoneProbeStub.Init(requestPtr, reinterpret_cast<intptr_t>(location) + 9);

		hook::nop(location, 9);
		hook::jump(location, zoneProbeStub.GetCode());
	}

	OnKillNetworkDone.Connect([]()
	{
		ClearAmvZoneFiles();
	});
});
