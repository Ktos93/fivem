#include "StdInc.h"

#include <Hooking.h>

#include <Streaming.h>
#ifdef GTA_FIVE
#include <EntitySystem.h>
#endif

#include <scrBind.h>

#include <CrossBuildRuntime.h>

#ifdef GTA_FIVE
#define RAGE_FORMATS_GAME five
#define RAGE_FORMATS_GAME_FIVE
#elif defined(IS_RDR3)
#define RAGE_FORMATS_GAME rdr3
#define RAGE_FORMATS_GAME_RDR3
#endif

#define RAGE_FORMATS_IN_GAME
#include <gtaDrawable.h>

#include <Resource.h>
#include <fxScripting.h>
#include <VFSManager.h>
#include <VFSWin32.h>

#include <grcTexture.h>

#ifdef GTA_FIVE
#include <RageParser.h>
#endif

#include <wrl.h>
#include <wincodec.h>

#include <shlwapi.h>
#include <botan/base64.h>

#include <nutsnbolts.h>

#define WANT_CEF_INTERNALS
#include <CefOverlay.h>

#ifdef GTA_FIVE
#include <NetLibrary.h>

#include <ResourceCacheDevice.h>
#include <ResourceManager.h>
#include <json.hpp>

#include <atPool.h>

#include <skyr/url.hpp>

#include <concurrent_unordered_set.h>
#endif

#include "FormData.h"

using Microsoft::WRL::ComPtr;

using RuntimeDictionary = rage::RAGE_FORMATS_GAME::pgDictionary<rage::grcTexture>;

static hook::cdecl_stub<RuntimeDictionary*(void*, int)> textureDictionaryCtor([]()
{
#ifdef GTA_FIVE
	return hook::get_call(hook::get_pattern("E8 ? ? ? ? 48 8B F8 EB 02 33 FF 4C 8D 3D"));
#elif defined(IS_RDR3)
	// PedShotManager::InitOutputTxd -> pgDictionary capacity ctor (0x1404F834C).
	return hook::get_call(hook::get_pattern("E8 ? ? ? ? 48 8B F0 EB ? 48 8B F5 48 89 B7"));
#endif
});

class RuntimeTex : public std::enable_shared_from_this<RuntimeTex>
{
	friend class RuntimeTxd;
public:
	RuntimeTex(const char* name, int width, int height);
#ifdef GTA_FIVE
	RuntimeTex(rage::grcTexture* texture, const void* data, size_t size);
#endif

	RuntimeTex(rage::grcTexture* texture, bool owned = true);

	virtual ~RuntimeTex();

	int GetWidth();

	int GetHeight();

	int GetPitch();

	void SetPixel(int x, int y, int r, int g, int b, int a);

	bool SetPixelData(const void* data, size_t length);

	bool LoadImage(const char* fileName);

	void Commit();

	inline void SetReferenceData(fwRefContainer<fwRefCountable> reference)
	{
		m_reference = reference;
	}

	inline rage::grcTexture* GetTexture()
	{
		return m_texture;
	}

	void SetTexture(rage::grcTexture* texture);

private:
	rage::grcTexture* m_texture = nullptr;

	fwRefContainer<fwRefCountable> m_reference;

#ifdef IS_RDR3
	int m_width = 0;
	int m_height = 0;
#endif
	int m_pitch = 0;

	std::vector<uint8_t> m_backingPixels;

	bool m_owned = false;
};

class RuntimeTxd : public std::enable_shared_from_this<RuntimeTxd>
{
public:
	RuntimeTxd(const char* name);
#ifdef IS_RDR3
	~RuntimeTxd();
#endif

	std::shared_ptr<RuntimeTex> CreateTexture(const char* name, int width, int height);

	std::shared_ptr<RuntimeTex> CreateTextureFromImage(const char* name, const char* fileName);

	std::shared_ptr<RuntimeTex> CreateTextureFromDui(const char* name, const char* duiHandle);

private:
	void EnsureTxd();
#ifdef IS_RDR3
	void ReleaseTxd();
	void BindResourceStop();
	std::mutex m_textureMutex;
	bool m_stopBound = false;
#endif
	void AddTexture(const char* name, const std::shared_ptr<RuntimeTex>& texture);

private:
	uint32_t m_txdIndex = -1;
	std::string m_name;

	std::unordered_map<std::string, std::shared_ptr<RuntimeTex>> m_textures;

	RuntimeDictionary* m_txd = nullptr;
};

#ifdef GTA_FIVE
RuntimeTex::RuntimeTex(const char* name, int width, int height)
	: m_owned(true)
{
	rage::grcManualTextureDef textureDef;
	memset(&textureDef, 0, sizeof(textureDef));
	textureDef.isStaging = 1;
	textureDef.usage = 1;
	textureDef.arraySize = 1;

	m_texture = rage::grcTextureFactory::getInstance()->createManualTexture(width, height, 2, nullptr, true, &textureDef);

	rage::grcLockedTexture lockedTexture;

	if (m_texture->Map(0, 0, &lockedTexture, rage::grcLockFlags::WriteDiscard))
	{
		auto newSize = static_cast<std::vector<uint8_t>::size_type>(lockedTexture.pitch) * lockedTexture.height;
		memset(lockedTexture.pBits, 0, newSize);
		m_backingPixels.resize(newSize);

		m_pitch = lockedTexture.pitch;

		m_texture->Unmap(&lockedTexture);
	}
}

RuntimeTex::RuntimeTex(rage::grcTexture* texture, const void* data, size_t size)
	: m_texture(texture), m_owned(true)
{
	m_backingPixels.resize(size);
	memcpy(&m_backingPixels[0], data, m_backingPixels.size());
}

#else
RuntimeTex::RuntimeTex(const char* name, int width, int height)
	: m_width(width), m_height(height), m_pitch(width * 4), m_owned(true)
{
	m_backingPixels.resize((size_t)m_pitch * height);
}

static rage::grcTexture* CreateTextureImage(int width, int height, int pitch, const std::vector<uint8_t>& pixels)
{
	rage::grcTextureReference reference{};
	reference.width = width;
	reference.height = height;
	reference.depth = 1;
	reference.stride = pitch;
	reference.format = 11; // grcImage BGRA8, as used by NUI.
	reference.pixelData = const_cast<uint8_t*>(pixels.data());
	return rage::grcTextureFactory::getInstance()->createImage(&reference, nullptr);
}
#endif

RuntimeTex::RuntimeTex(rage::grcTexture* texture, bool owned)
	: m_texture(texture), m_owned(owned)
{
	m_backingPixels.resize(0);
}

RuntimeTex::~RuntimeTex()
{
	if (m_owned)
	{
#ifdef IS_RDR3
		nui::EnqueueRenderWork([texture = m_texture]() { delete texture; });
#else
		delete m_texture;
#endif
		m_texture = nullptr;
	}
}

#ifdef IS_RDR3
// The 24-byte helper used by PedShotManager at 0x14055BB58. An empty list
// disables its collection of previously retired slots; the store is at +0x10.
struct RuntimeTxdRegistrar
{
	void* slots = nullptr;
	uint16_t count = 0;
	uint16_t capacity = 0;
	uint32_t padding = 0;
	void* store = nullptr;
};
static_assert(sizeof(RuntimeTxdRegistrar) == 24);
static_assert(sizeof(RuntimeDictionary) == 64);

static void* GetRuntimeTxdStore()
{
	static auto store = hook::get_address<void*>(hook::get_pattern("48 8D 0D ? ? ? ? 48 89 28"), 3, 7);
	return store;
}

static hook::cdecl_stub<uint32_t*(RuntimeTxdRegistrar*, uint32_t*, uint32_t, RuntimeDictionary*)> registerRuntimeTxd([]()
{
	// 0x1405039CC: allocate slot (+0x170), install live dictionary (+0x160),
	// SetDoNotDefrag and acquire a store reference. SetResource builds RSCs instead.
	return hook::get_call(hook::get_pattern("E8 ? ? ? ? 8B 08 89 8F ? ? ? ? 83 F9"));
});

template<typename Result, typename... Args>
static Result CallTxdStore(size_t offset, Args... args)
{
	void* store = GetRuntimeTxdStore();
	return reinterpret_cast<Result(*)(void*, Args...)>((*reinterpret_cast<void***>(store))[offset / sizeof(void*)])(store, args...);
}

RuntimeTxd::~RuntimeTxd()
{
	ReleaseTxd();
}

void RuntimeTxd::ReleaseTxd()
{
	std::lock_guard lock(m_textureMutex);
	if (!m_txd)
	{
		return;
	}
	// Release on the NUI render queue, after preceding texture operations.
	nui::EnqueueRenderWork([dictionary = m_txd, index = m_txdIndex, textures = std::move(m_textures)]()
	{
		for (uint16_t i = 0; i < dictionary->GetCount(); i++)
		{
			dictionary->SetAt(i, nullptr); // RuntimeTex/GITexture owns the images.
		}
		CallTxdStore<void>(0xB0, index);
		if (CallTxdStore<int>(0xC0, index) == 0)
		{
			CallTxdStore<void>(0x40, index);
		}
		for (auto& [name, texture] : textures)
		{
			if (texture->m_owned)
			{
				delete texture->m_texture;
			}
			texture->m_texture = nullptr;
			texture->m_reference = nullptr;
		}
	});
	m_txd = nullptr;
	m_txdIndex = -1;
}
#endif

int RuntimeTex::GetWidth()
{
#ifdef GTA_FIVE
	return m_texture ? m_texture->GetWidth() : 0;
#else
	return m_owned ? m_width : (m_texture ? m_texture->GetWidth() : 0);
#endif
}

int RuntimeTex::GetHeight()
{
#ifdef GTA_FIVE
	return m_texture ? m_texture->GetHeight() : 0;
#else
	return m_owned ? m_height : (m_texture ? m_texture->GetHeight() : 0);
#endif
}

int RuntimeTex::GetPitch()
{
	return m_pitch;
}

void RuntimeTex::SetTexture(rage::grcTexture* texture)
{
	if (!m_texture)
	{
		m_texture = texture;
	}
}

void RuntimeTex::SetPixel(int x, int y, int r, int g, int b, int a)
{
#ifdef IS_RDR3
	if (m_backingPixels.empty() || x < 0 || y < 0 || x >= GetWidth() || y >= GetHeight())
	{
		return;
	}
#endif
	auto offset = (y * m_pitch) + (x * 4);

	if (offset < 0 || offset > m_backingPixels.size() - 4)
	{
		return;
	}

	auto start = &m_backingPixels[offset];

	start[3] = a;
	start[2] = r;
	start[1] = g;
	start[0] = b;
}

bool RuntimeTex::SetPixelData(const void* data, size_t length)
{
	if (length != m_backingPixels.size())
	{
		return false;
	}

#ifdef GTA_FIVE
	if (!m_texture)
	{
		return false;
	}

	rage::grcLockedTexture lockedTexture;

	if (m_texture->Map(0, 0, &lockedTexture, rage::grcLockFlags::WriteDiscard))
	{
		memcpy(lockedTexture.pBits, data, length);
		memcpy(m_backingPixels.data(), data, length);
		m_texture->Unmap(&lockedTexture);
	}
#else
	if (!data || m_backingPixels.empty())
	{
		return false;
	}
	memcpy(m_backingPixels.data(), data, length);

	Commit();
#endif

	return true;
}

void RuntimeTex::Commit()
{
#ifdef GTA_FIVE
	rage::grcLockedTexture lockedTexture;

	if (m_texture && m_texture->Map(0, 0, &lockedTexture, rage::grcLockFlags::WriteDiscard))
	{
		memcpy(lockedTexture.pBits, m_backingPixels.data(), m_backingPixels.size());
		m_texture->Unmap(&lockedTexture);
	}
#elif defined(IS_RDR3)
	if (m_backingPixels.empty())
	{
		return;
	}
	// Capture the committed image; scripts may edit the next frame's pixels immediately.
	nui::EnqueueRenderWork([self = shared_from_this(), pixels = m_backingPixels]()
	{
		if (self->m_texture)
		{
			auto staging = CreateTextureImage(self->m_width, self->m_height, self->m_pitch, pixels);
			if (staging)
			{
				rage::sga::CopyTexture(staging, self->m_texture);
				delete staging;
			}
		}
	});
#endif
}

RuntimeTxd::RuntimeTxd(const char* name)
	: m_name(name)
{
	EnsureTxd();
}

void RuntimeTxd::EnsureTxd()
{
#ifdef GTA_FIVE
	streaming::Manager* streaming = streaming::Manager::GetInstance();
	auto txdStore = streaming->moduleMgr.GetStreamingModule("ytd");

	txdStore->FindSlotFromHashKey(&m_txdIndex, m_name.c_str());

	if (m_txdIndex != 0xFFFFFFFF)
	{
		auto& entry = streaming->Entries[txdStore->baseIdx + m_txdIndex];

		if (!entry.handle)
		{
			void* memoryStub = rage::GetAllocator()->Allocate(sizeof(RuntimeDictionary), 16, 0);
			m_txd = textureDictionaryCtor(memoryStub, 1);

			streaming::strAssetReference ref;
			ref.asset = m_txd;

			txdStore->SetResource(m_txdIndex, ref);
			entry.flags = (512 << 8) | 1;
			entry.flags |= (0x20000000); // SetDoNotDefrag

			txdStore->AddRef(m_txdIndex);
		}
	}
#elif defined(IS_RDR3)
	void* memory = rage::GetAllocator()->Allocate(sizeof(RuntimeDictionary), 16, 0);
	m_txd = textureDictionaryCtor(memory, 1);
	RuntimeTxdRegistrar registrar;
	registrar.store = GetRuntimeTxdStore();
	registerRuntimeTxd(&registrar, &m_txdIndex, HashString(m_name), m_txd);
	if (m_txdIndex == uint32_t(-1))
	{
		// RDR3's formats datBase is a disk layout, not a C++ virtual base.
		reinterpret_cast<void(*)(void*, int)>((*reinterpret_cast<void***>(m_txd))[0])(m_txd, 1);
		m_txd = nullptr;
	}
#endif
}

#ifdef IS_RDR3
void RuntimeTxd::BindResourceStop()
{
	if (!m_stopBound)
	{
		fx::OMPtr<IScriptRuntime> runtime;
		if (FX_SUCCEEDED(fx::GetCurrentScriptRuntime(&runtime)))
		{
			auto resource = reinterpret_cast<fx::Resource*>(runtime->GetParentObject());
			resource->OnStop.Connect([weakTxd = weak_from_this()]()
			{
				if (auto txd = weakTxd.lock())
				{
					txd->ReleaseTxd();
				}
			});
			m_stopBound = true;
		}
	}
}
#endif

void RuntimeTxd::AddTexture(const char* name, const std::shared_ptr<RuntimeTex>& texture)
{
#ifdef GTA_FIVE
	m_txd->Add(name, texture->GetTexture());
#elif defined(IS_RDR3)
	BindResourceStop();
	nui::EnqueueRenderWork([txd = weak_from_this(), texture, name = std::string(name), pixels = texture->m_backingPixels]()
	{
		if (auto locked = txd.lock())
		{
			std::lock_guard lock(locked->m_textureMutex);
			if (locked->m_txd)
			{
				texture->m_texture = CreateTextureImage(texture->m_width, texture->m_height, texture->m_pitch, pixels);
				if (texture->GetTexture())
				{
					locked->m_txd->Add(name.c_str(), texture->GetTexture());
				}
			}
		}
	});
#endif
}

std::shared_ptr<RuntimeTex> RuntimeTxd::CreateTexture(const char* name, int width, int height)
{
	if (!m_txd)
	{
		return nullptr;
	}

	if (m_textures.find(name) != m_textures.end())
	{
		return nullptr;
	}

	if (width <= 0 || width > 8192)
	{
		return nullptr;
	}

	if (height <= 0 || height > 8192)
	{
		return nullptr;
	}

	auto tex = std::make_shared<RuntimeTex>(name, width, height);
	AddTexture(name, tex);

	m_textures[name] = tex;

	return tex;
}

extern void TextureReplacement_OnTextureCreate(const std::string& txd, const std::string& txn);

// TODO: we need a 'common' place for this
static std::mutex nextFrameLock;
static std::queue<std::function<void()>> nextFrameQueue;

static void OnNextMainFrame(std::function<void()>&& fn)
{
	std::unique_lock _(nextFrameLock);
	nextFrameQueue.push(std::move(fn));
}

std::shared_ptr<RuntimeTex> RuntimeTxd::CreateTextureFromDui(const char* name, const char* duiHandle)
{
	if (!m_txd)
	{
		return nullptr;
	}

	if (m_textures.find(name) != m_textures.end())
	{
		return nullptr;
	}

	auto texture = nui::GetWindowTexture(duiHandle);
	if (!texture.GetRef())
	{
		return nullptr;
	}
	auto tex = std::make_shared<RuntimeTex>(nullptr, false);
	tex->SetReferenceData(texture);

#ifdef IS_RDR3
	BindResourceStop();
#endif

	texture->WithHostTexture([weakTxd = weak_from_this(), name = std::string{ name }, tex](void* hostTexture)
	{
		auto txd = weakTxd.lock();
		if (!txd)
		{
			return;
		}
#ifdef IS_RDR3
		std::lock_guard lock(txd->m_textureMutex);
#endif
		if (!txd->m_txd || !hostTexture)
		{
			return;
		}
		auto texture = (rage::grcTexture*)hostTexture;
		tex->SetTexture(texture);
		txd->m_txd->Add(HashString(name), tex->GetTexture());

#ifdef GTA_FIVE
		OnNextMainFrame([txdName = txd->m_name, name = std::move(name)]()
		{
			TextureReplacement_OnTextureCreate(txdName, name);
		});
#endif
	});

	m_textures[name] = tex;

	return tex;
}

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

static ComPtr<IWICImagingFactory> g_imagingFactory;

static ComPtr<IWICBitmapSource> ImageToBitmapSource(std::string_view fileName)
{
	if (!g_imagingFactory)
	{
		HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)g_imagingFactory.GetAddressOf());
	}

	ComPtr<IWICBitmapSource> source;
	ComPtr<IStream> stream;

	std::string fileNameString(fileName);

	if (fileNameString.find("data:") == 0)
	{
		auto f = fileNameString.find("base64,");

		if (f == std::string::npos)
		{
			return {};
		}

		fileNameString = fileNameString.substr(f + 7);

		std::string decodedURL;
		net::UrlDecode(fileNameString, decodedURL, false);

		decodedURL.erase(std::remove_if(decodedURL.begin(), decodedURL.end(), [](char c)
						 {
							 return std::isspace<char>(c, std::locale::classic());
						 }),
		decodedURL.end());

		size_t length = decodedURL.length();
		size_t paddingNeeded = 4 - (length % 4);

		if ((paddingNeeded == 1 || paddingNeeded == 2) && decodedURL[length - 1] != '=')
		{
			decodedURL.resize(length + paddingNeeded, '=');
		}

		auto imageData = Botan::base64_decode(decodedURL, false);

		stream = SHCreateMemStream(imageData.data(), imageData.size());
	}
	else
	{
		fx::OMPtr<IScriptRuntime> runtime;

		if (!FX_SUCCEEDED(fx::GetCurrentScriptRuntime(&runtime)))
		{
			return {};
		}

		fx::Resource* resource = reinterpret_cast<fx::Resource*>(runtime->GetParentObject());

		stream = vfs::CreateComStream(vfs::OpenRead(fmt::sprintf("%s/%s", resource->GetPath(), fileName)));
	}

	ComPtr<IWICBitmapDecoder> decoder;

	HRESULT hr = g_imagingFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());

	if (SUCCEEDED(hr))
	{
		ComPtr<IWICBitmapFrameDecode> frame;

		hr = decoder->GetFrame(0, frame.GetAddressOf());

		if (SUCCEEDED(hr))
		{
			// try to convert to a pixel format we like
			frame.As(&source);
		}
	}

	return source;
}

std::shared_ptr<RuntimeTex> RuntimeTxd::CreateTextureFromImage(const char* name, const char* fileName)
{
	if (!m_txd)
	{
		return nullptr;
	}

#ifdef IS_RDR3
	if (m_textures.find(name) != m_textures.end())
	{
		return nullptr;
	}
#endif

	if (auto source = ImageToBitmapSource(fileName))
	{
		ComPtr<IWICBitmapSource> convertedSource;

		UINT width = 0, height = 0;
		source->GetSize(&width, &height);

		// try to convert to a pixel format we like
		HRESULT hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, source.Get(), convertedSource.GetAddressOf());

		if (SUCCEEDED(hr))
		{
			source = convertedSource;
		}

#ifdef IS_RDR3
		if (width == 0 || height == 0 || width > 8192 || height > 8192)
		{
			return nullptr;
		}
#endif
		// create a pixel data buffer
		std::unique_ptr<uint32_t[]> pixelData(new uint32_t[width * height]);

		hr = source->CopyPixels(nullptr, width * 4, width * height * 4, reinterpret_cast<BYTE*>(pixelData.get()));

		if (SUCCEEDED(hr))
		{
#ifdef GTA_FIVE
			rage::grcTextureReference reference;
			memset(&reference, 0, sizeof(reference));
			reference.width = width;
			reference.height = height;
			reference.depth = 1;
			reference.stride = width * 4;
			reference.format = 11; // should correspond to DXGI_FORMAT_B8G8R8A8_UNORM
			reference.pixelData = (uint8_t*)pixelData.get();

			auto tex = std::make_shared<RuntimeTex>(rage::grcTextureFactory::getInstance()->createImage(&reference, nullptr), pixelData.get(), width * height * 4);
#elif defined(IS_RDR3)
			auto tex = std::make_shared<RuntimeTex>(name, width, height);
			memcpy(tex->m_backingPixels.data(), pixelData.get(), width * height * 4);
#endif
			AddTexture(name, tex);
			m_textures[name] = tex;

			return tex;
		}
	}

	return nullptr;
}

bool RuntimeTex::LoadImage(const char* fileName)
{
#ifdef GTA_FIVE
	if (!m_texture)
#else
	if (m_backingPixels.empty())
#endif
	{
		return false;
	}

	auto completion = [self = shared_from_this(), this](const ComPtr<IWICBitmapSource>& bitmapSource)
	{
		auto source = bitmapSource;
		ComPtr<IWICBitmapSource> convertedSource;

		// try to convert to a pixel format we like
		HRESULT hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, source.Get(), convertedSource.GetAddressOf());

		if (SUCCEEDED(hr))
		{
			source = convertedSource;
		}

		// set up the pixel data buffer
		UINT width = 0, height = 0;
		source->GetSize(&width, &height);

		size_t length = (size_t(width) * size_t(height) * 4);

		if (length == m_backingPixels.size())
		{
			if (SUCCEEDED(source->CopyPixels(nullptr, width * 4, width * height * 4, reinterpret_cast<BYTE*>(&m_backingPixels[0]))))
			{
				Commit();

				return true;
			}
		}

		return false;
	};

	if (auto source = ImageToBitmapSource(fileName))
	{
		UINT width = 0, height = 0;
		source->GetSize(&width, &height);

		if (width == GetWidth() && height == GetHeight())
		{
			return completion(source);
		}
		else
		{
			struct Item
			{
				ComPtr<IWICBitmapSource> bitmap;
				int targetWidth;
				int targetHeight;
				decltype(completion) cb;

				Item(ComPtr<IWICBitmapSource>&& bitmap, int targetWidth, int targetHeight, decltype(completion)&& cb)
					: bitmap(std::move(bitmap)), targetWidth(targetWidth), targetHeight(targetHeight), cb(std::move(cb))
				{

				}

				void Work()
				{
					ComPtr<IWICBitmapScaler> scaler;

					if (SUCCEEDED(g_imagingFactory->CreateBitmapScaler(&scaler)))
					{
						if (SUCCEEDED(scaler->Initialize(
							bitmap.Get(),
							targetWidth,
							targetHeight,
							WICBitmapInterpolationModeFant)))
						{
							ComPtr<IWICBitmapSource> outBitmap;
							scaler.As(&outBitmap);

							OnNextMainFrame([cb = std::move(cb), bitmap = std::move(outBitmap)]()
							{
								cb(bitmap);
							});
						}
					}
				}

				static DWORD WINAPI StaticWork(LPVOID arg)
				{
					auto self = static_cast<Item*>(arg);
					self->Work();
					delete self;

					return 0;
				}
			};

			auto workItem = new Item(std::move(source), GetWidth(), GetHeight(), std::move(completion));
			QueueUserWorkItem(&Item::StaticWork, workItem, 0);

			return true;
		}
	}

	return false;
}

#ifdef GTA_FIVE
#define VFS_GET_RAGE_PAGE_FLAGS 0x20001

struct GetRagePageFlagsExtension
{
	const char* fileName; // in
	int version;
	rage::ResourceFlags flags; // out
};

void DLL_IMPORT CfxCollection_AddStreamingFileByTag(const std::string& tag, const std::string& fileName, rage::ResourceFlags flags);

static hook::cdecl_stub<void(fwArchetype*)> registerArchetype([]()
{
	return hook::get_pattern("48 8B D9 8A 49 60 80 F9", -11);
});

static void* MakeStructFromMsgPack(uint32_t hash, const std::map<std::string, msgpack::object>& data, void* old = nullptr, bool keep = false);

static bool FillStructure(rage::parStructure* structure, const std::function<bool(rage::parMember* member)>& fn)
{
	if (structure->m_baseClass)
	{
		if (!FillStructure(structure->m_baseClass, fn))
		{
			return false;
		}
	}

	for (auto& member : structure->m_members)
	{
		if (!fn(member))
		{
			return false;
		}
	}

	return true;
}

template<typename T>
static bool SetValue(void* ptr, const msgpack::object& obj)
{
	*(T*)ptr = obj.as<T>();

	return true;
}

struct Vector2
{
	float x;
	float y;

	Vector2(float x, float y, float z, float w)
		: x(x), y(y)
	{

	}
};

struct Vector3f
{
	float x;
	float y;
	float z;

	Vector3f(float x, float y, float z, float w)
		: x(x), y(y), z(z)
	{

	}
};

struct Vector4
{
	float x;
	float y;
	float z;
	float w;

	Vector4(float x, float y, float z, float w)
		: x(x), y(y), z(z), w(w)
	{

	}
};

template<typename T>
static bool SetVector(void* ptr, const msgpack::object& obj)
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 0.0f;

	if (obj.type == msgpack::type::EXT)
	{
		if (obj.via.ext.type() == 20 && obj.via.ext.size == 8) // vector2
		{
			x = *(float*)(obj.via.ext.data() + 0);
			y = *(float*)(obj.via.ext.data() + 4);
		}
		else if (obj.via.ext.type() == 21 && obj.via.ext.size == 12) // vector3
		{
			x = *(float*)(obj.via.ext.data() + 0);
			y = *(float*)(obj.via.ext.data() + 4);
			z = *(float*)(obj.via.ext.data() + 8);
		}
		else if ((obj.via.ext.type() == 22 || obj.via.ext.type() == 23) && obj.via.ext.size == 16) // vector4/quat
		{
			x = *(float*)(obj.via.ext.data() + 0);
			y = *(float*)(obj.via.ext.data() + 4);
			z = *(float*)(obj.via.ext.data() + 8);
			w = *(float*)(obj.via.ext.data() + 12);
		}
	}
	else if (obj.type == msgpack::type::ARRAY)
	{
		std::vector<float> floats = obj.as<std::vector<float>>();

		if (floats.size() >= 2)
		{
			x = floats[0];
			y = floats[1];
		}

		if (floats.size() >= 3)
		{
			z = floats[2];
		}

		if (floats.size() >= 4)
		{
			w = floats[3];
		}
	}

	*(T*)ptr = T{ x, y, z, w };

	return true;
}

static bool SetString(void* ptr, const msgpack::object& obj)
{
	if (obj.type == msgpack::type::STR || obj.type == msgpack::type::BIN)
	{
		*(uint32_t*)ptr = HashString(obj.as<std::string>().c_str());
	}
	else if (obj.type == msgpack::type::POSITIVE_INTEGER || obj.type == msgpack::type::NEGATIVE_INTEGER)
	{
		*(uint32_t*)ptr = obj.as<int32_t>();
	}

	return true;
}

static bool SetValueEnum(void* ptr, const msgpack::object& obj, rage::parEnumDefinition* enum_)
{
	uint32_t str = 0;

	if (obj.type == msgpack::type::STR || obj.type == msgpack::type::BIN)
	{
		str = HashRageString(obj.as<std::string>().c_str());
	}
	else if (obj.type == msgpack::type::POSITIVE_INTEGER || obj.type == msgpack::type::NEGATIVE_INTEGER)
	{
		str = obj.as<int32_t>();
	}

	for (auto p = enum_->fields; p->index != 0xFFFFFFFF; p++)
	{
		if (p->hash == str)
		{
			*(uint32_t*)ptr = p->index;
			return true;
		}
	}

	return false;
}

static bool SetStruct(void* ptr, const msgpack::object& obj, rage::parMemberDefinition* member)
{
	if (obj.type != msgpack::type::MAP)
	{
		return false;
	}

	auto structData = obj.as<std::map<std::string, msgpack::object>>();

	if (member->structType == rage::parStructType::Inline)
	{
		MakeStructFromMsgPack(member->structure->m_nameHash, structData, ptr);
	}
	else
	{
		*(void**)ptr = MakeStructFromMsgPack(member->structure->m_nameHash, structData);
	}

	return true;
}

template<typename TIndex>
struct atArrayBase
{
	void* offset;
	TIndex count;
	TIndex size;

	atArrayBase(int count, int size)
		: count(count), size(size)
	{
		offset = rage::GetAllocator()->allocate(static_cast<size_t>(count) * size, 16, 0);
	}

	~atArrayBase()
	{
		if (offset)
		{
			rage::GetAllocator()->free(offset);
			offset = nullptr;
		}
	}
};

using atArray_16 = atArrayBase<uint16_t>;
using atArray_32 = atArrayBase<uint32_t>;

static bool SetFromMsgPack(rage::parMember* memberBase, void* structVal, const msgpack::object& value);

static bool SetArray(void* ptr, const msgpack::object& value, rage::parMember* member)
{
	if (value.type != msgpack::type::ARRAY)
	{
		return false;
	}

	// set up the array
	auto arrayData = value.as<std::vector<msgpack::object>>();

	char* arrayBase = nullptr;
	uint32_t arrayElemSize = 0;

	switch (member->m_definition->arrayType)
	{
	case rage::parArrayType::atArray:
		*(atArray_16*)ptr = atArray_16{int(arrayData.size()), int(member->m_definition->arrayElemSize)};
		arrayBase = (char*)(((atArray_16*)ptr)->offset);
		arrayElemSize = member->m_definition->arrayElemSize;

		break;
	case rage::parArrayType::FixedTrailingCount:
		trace("Unhandled array type: FixedTrailingCount\n");
		return false;
	case rage::parArrayType::Fixed:
		arrayBase = (char*)ptr;
		arrayElemSize = member->m_definition->arrayElemSize;
		break;
	case rage::parArrayType::FixedPointer:
		trace("Unhandled array type: FixedPointer\n");
		return false;
	case rage::parArrayType::Fixed_2:
		arrayBase = (char*)ptr;
		arrayElemSize = member->m_definition->arrayElemSize;
		break;
	case rage::parArrayType::atArray32:
		*(atArray_32*)ptr = atArray_32{ int(arrayData.size()), int(member->m_definition->arrayElemSize) };
		arrayBase = (char*)(((atArray_32*)ptr)->offset);
		arrayElemSize = member->m_definition->arrayElemSize;

		break;
	case rage::parArrayType::TrailerInt:
		trace("Unhandled array type: TrailerInt\n");
		return false;
	case rage::parArrayType::TrailerByte:
		trace("Unhandled array type: TrailerByte\n");
		return false;
	case rage::parArrayType::TrailerShort:
		trace("Unhandled array type: TrailerShort\n");
		return false;
	default:
		break;

	}

	size_t offset = 0;
	for (const auto& entry : arrayData)
	{
		void* entryPtr = arrayBase + offset;
		offset += arrayElemSize;

		if (!SetFromMsgPack(member->m_arrayDefinition, entryPtr, entry))
		{
			return false;
		}
	}

	return true;
}

void* MakeStructFromMsgPack(const char* structType, const std::map<std::string, msgpack::object>& data, void* old = nullptr, bool keep = false)
{
	return MakeStructFromMsgPack(HashRageString(structType), data, old, keep);
}

static bool SetFromMsgPack(rage::parMember* memberBase, void* structVal, const msgpack::object& value)
{
	auto member = memberBase->m_definition;

	switch (member->type)
	{
	case rage::parMemberType::Float:
		return SetValue<float>(structVal, value);
	case rage::parMemberType::String:
		return SetString(structVal, value);
	case rage::parMemberType::Vector3:
		return SetVector<Vector3f>(structVal, value);
	case rage::parMemberType::Bool:
		return SetValue<bool>(structVal, value);
	case rage::parMemberType::Int8:
		return SetValue<int8_t>(structVal, value);
	case rage::parMemberType::UInt8:
		return SetValue<uint8_t>(structVal, value);
	case rage::parMemberType::Int16:
		return SetValue<int16_t>(structVal, value);
	case rage::parMemberType::UInt16:
		return SetValue<uint16_t>(structVal, value);
	case rage::parMemberType::Int32:
		return SetValue<int32_t>(structVal, value);
	case rage::parMemberType::UInt32:
		return SetValue<uint32_t>(structVal, value);
	case rage::parMemberType::Vector2:
		return SetVector<Vector2>(structVal, value);
	case rage::parMemberType::Vector4:
		return SetVector<Vector4>(structVal, value);
	case rage::parMemberType::Struct:
		return SetStruct(structVal, value, member);
	case rage::parMemberType::Array:
		return SetArray(structVal, value, memberBase);
	case rage::parMemberType::Enum:
		return SetValueEnum(structVal, value, member->enumData);
	case rage::parMemberType::Bitset:
		break;
	case rage::parMemberType::Map:
		break;
	case rage::parMemberType::Matrix4x3:
		break;
	case rage::parMemberType::Matrix4x4:
		break;
	case rage::parMemberType::Vector2_Padded:
		return SetVector<Vector4>(structVal, value);
	case rage::parMemberType::Vector3_Padded:
		return SetVector<Vector4>(structVal, value);
	case rage::parMemberType::Vector4_Padded:
		return SetVector<Vector4>(structVal, value);
	case rage::parMemberType::Matrix3x4:
		break;
	case rage::parMemberType::Matrix4x3_2:
		break;
	case rage::parMemberType::Matrix4x4_2:
		break;
	case rage::parMemberType::Vector1_Padded:
		break;
	case rage::parMemberType::Flag1_Padded:
		break;
	case rage::parMemberType::Flag4_Padded:
		break;
	case rage::parMemberType::Int32_64:
		return SetValue<int64_t>(structVal, value);
	case rage::parMemberType::Int32_U64:
		return SetValue<uint64_t>(structVal, value);
	case rage::parMemberType::Half:
		trace("Unhandled parMemberType::Half\n");
		break;
	case rage::parMemberType::Int64:
		return SetValue<int64_t>(structVal, value);
	case rage::parMemberType::UInt64:
		return SetValue<uint64_t>(structVal, value);
	case rage::parMemberType::Double:
		return SetValue<double>(structVal, value);
	default:
		break;
	}

	return true;
}

static void* MakeStructFromMsgPack(uint32_t hash, const std::map<std::string, msgpack::object>& data, void* old, bool keep)
{
	std::string structTypeReal;

	{
		auto typeIt = data.find("__type");

		if (typeIt != data.end())
		{
			structTypeReal = typeIt->second.as<std::string>();
		}
	}

	auto structDef = rage::GetStructureDefinition(hash);

	if (!structTypeReal.empty())
	{
		structDef = rage::GetStructureDefinition(structTypeReal.c_str());
	}

	if (!structDef)
	{
		return nullptr;
	}

	void* retval = nullptr;

	if (old)
	{
		if (keep)
		{
			retval = old;
		}
		else
		{
			retval = structDef->m_placementNew(old);
		}
	}
	else
	{
		retval = structDef->m_new();
	}

	std::map<uint32_t, std::reference_wrapper<const msgpack::object>> mappedData;

	for (auto& entry : data)
	{
		mappedData.emplace(HashRageString(entry.first.c_str()), entry.second);
	}

	if (retval)
	{
		FillStructure(structDef, [retval, &mappedData](rage::parMember* memberBase)
		{
			auto member = memberBase->m_definition;
			auto structVal = (char*)retval + member->offset;

			auto it = mappedData.find(member->hash);

			if (it == mappedData.end())
			{
				return true;
			}

			const msgpack::object& value = it->second;

			return SetFromMsgPack(memberBase, structVal, value);
		});
	}

	return retval;
}

static hook::cdecl_stub<CMapDataContents*()> makeMapDataContents([]()
{
	return hook::pattern("48 00 00 00 E8 ? ? ? ? 48 8B D8 48 85 C0 74 14").count(1).get(0).get<void>(-7);
});

static hook::cdecl_stub<void(CMapDataContents*, CMapData*, bool, bool)> addToScene([]()
{
	return hook::pattern("48 83 EC 50 83 79 18 00 0F 29 70 C8 41 8A F1").count(1).get(0).get<void>(-0x18);
});

static hook::cdecl_stub<void*(fwEntityDef*, int fileIdx, fwArchetype* archetype, uint64_t* archetypeUnk)> fwEntityDef__instantiate([]()
{
	return hook::get_call(hook::pattern("4C 8D 4C 24 40 4D 8B C6 41 8B D7 48 8B CF").count(1).get(0).get<void>(14));
});

#endif

static InitFunction initFunction([]()
{
	OnMainGameFrame.Connect([]
	{
		if (nextFrameQueue.empty())
		{
			return;
		}

		decltype(nextFrameQueue) q;

		{
			std::unique_lock _(nextFrameLock);
			q = std::move(nextFrameQueue);
		}

		while (!q.empty())
		{
			q.front()();
			q.pop();
		}
	});

	scrBindClass<RuntimeTxd>()
		.AddConstructor<const char*>("CREATE_RUNTIME_TXD")
		.AddMethod("CREATE_RUNTIME_TEXTURE", &RuntimeTxd::CreateTexture)
		.AddMethod("CREATE_RUNTIME_TEXTURE_FROM_IMAGE", &RuntimeTxd::CreateTextureFromImage)
		.AddMethod("CREATE_RUNTIME_TEXTURE_FROM_DUI_HANDLE", &RuntimeTxd::CreateTextureFromDui);

	scrBindClass<RuntimeTex>()
		.AddMethod("GET_RUNTIME_TEXTURE_WIDTH", &RuntimeTex::GetWidth)
		.AddMethod("GET_RUNTIME_TEXTURE_HEIGHT", &RuntimeTex::GetHeight)
		.AddMethod("GET_RUNTIME_TEXTURE_PITCH", &RuntimeTex::GetPitch)
		.AddMethod("SET_RUNTIME_TEXTURE_PIXEL", &RuntimeTex::SetPixel)
		.AddMethod("SET_RUNTIME_TEXTURE_ARGB_DATA", &RuntimeTex::SetPixelData)
		.AddMethod("SET_RUNTIME_TEXTURE_IMAGE", &RuntimeTex::LoadImage)
		.AddMethod("COMMIT_RUNTIME_TEXTURE", &RuntimeTex::Commit);

#ifdef GTA_FIVE
	fx::ScriptEngine::RegisterNativeHandler("REGISTER_ARCHETYPES", [](fx::ScriptContext& context)
	{
		fx::OMPtr<IScriptRuntime> runtime;

		std::string factoryRef = context.CheckArgument<const char*>(0);

		if (FX_SUCCEEDED(fx::GetCurrentScriptRuntime(&runtime)))
		{
			fx::Resource* resource = reinterpret_cast<fx::Resource*>(runtime->GetParentObject());

			if (resource)
			{
				msgpack::unpacked unpacked;
				auto factoryObject = resource->GetManager()->CallReferenceUnpacked<msgpack::object>(factoryRef, &unpacked);

				if (factoryObject.type != msgpack::type::ARRAY && factoryObject.type != msgpack::type::MAP)
				{
					throw std::runtime_error("Wrong type in REGISTER_ARCHETYPES.");
				}

				std::vector<std::map<std::string, msgpack::object>> archetypes =
					(factoryObject.type == msgpack::type::ARRAY) ?
						factoryObject.as<std::vector<std::map<std::string, msgpack::object>>>() :
						std::vector<std::map<std::string, msgpack::object>>{ factoryObject.as<std::map<std::string, msgpack::object>>() };

				for (const auto& archetypeData : archetypes)
				{
					fwArchetypeDef* archetypeDef = (fwArchetypeDef*)MakeStructFromMsgPack("CBaseArchetypeDef", archetypeData);

					// assume this is a CBaseModelInfo
					g_archetypeFactories->Get(1)->AddStorageBlock(archetypeDef->name, 1);

					fwArchetype* mi = g_archetypeFactories->Get(1)->CreateBaseItem(archetypeDef->name);

					mi->InitializeFromArchetypeDef(1390, archetypeDef, true);

					// TODO: clean up
					mi->streaming = 0;

					// register the archetype in the streaming module
					registerArchetype(mi);
				}
			}
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("REGISTER_STREAMING_FILE_FROM_CACHE", [](fx::ScriptContext& context)
	{
		std::string resourceName = context.CheckArgument<const char*>(0);
		std::string fileName = context.CheckArgument<const char*>(1);
		std::string dataString = context.CheckArgument<const char*>(2);

		auto resourceManager = fx::ResourceManager::GetCurrent();
		auto resource = resourceManager->GetResource(resourceName);

		if (!resource.GetRef())
		{
			throw std::runtime_error("No valid resource name in REGISTER_STREAMING_FILE_FROM_CACHE.");
			return;
		}

		auto json = nlohmann::json::parse(dataString);

		uint32_t version;
		uint32_t pagesPhysical;
		uint32_t pagesVirtual;

		if (json.value("isResource", false))
		{
			pagesPhysical = json.value<uint32_t>("rscPagesPhysical", 0);
			pagesVirtual = json.value<uint32_t>("rscPagesVirtual", 0);
			version = json.value<uint32_t>("rscVersion", 0);
		}
		else
		{
			version = 0;
			pagesPhysical = 0;
			pagesVirtual = json.value<uint32_t>("size", 0);
		}

		ResourceCacheEntryList::Entry rclEntry;
		rclEntry.basename = fileName;
		rclEntry.referenceHash = json.value("hash", "");
		rclEntry.remoteUrl = fmt::sprintf("https://%s/files/%s/%s", Instance<NetLibrary>::Get()->GetCurrentPeer().ToString(), resourceName, fileName);
		rclEntry.resourceName = resource->GetName();
		rclEntry.size = json.value("size", -1);
		rclEntry.extData = {
			{ "rscVersion", std::to_string(version) },
			{ "rscPagesPhysical", std::to_string(pagesPhysical) },
			{ "rscPagesVirtual", std::to_string(pagesVirtual) },
		};

		resource->GetComponent<ResourceCacheEntryList>()->AddEntry(rclEntry);

		rage::ResourceFlags flags{ pagesVirtual, pagesPhysical };

		// TODO: don't use compcache if not using the adhesive mounter!
		CfxCollection_AddStreamingFileByTag(resource->GetName(), fmt::sprintf("compcache:/%s/%s", resourceName, fileName), flags);
	});

	static concurrency::concurrent_unordered_set<std::string> g_readyStreamFiles;

	fx::ScriptEngine::RegisterNativeHandler("IS_STREAMING_FILE_READY", [](fx::ScriptContext& context)
	{
		std::string fileName = context.CheckArgument<const char*>(0);

		context.SetResult<int>(g_readyStreamFiles.find(fileName) != g_readyStreamFiles.end());
	});

	fx::ScriptEngine::RegisterNativeHandler("REGISTER_STREAMING_FILE_FROM_KVS", [](fx::ScriptContext& context)
	{
		std::string key = context.CheckArgument<const char*>(0);

		fx::OMPtr<IScriptRuntime> runtime;

		if (FX_FAILED(fx::GetCurrentScriptRuntime(&runtime)))
		{
			throw std::runtime_error("no current script runtime");
		}

		auto resource = reinterpret_cast<fx::Resource*>(runtime->GetParentObject());

		std::string fileName = fmt::sprintf("kvs:/%s/%s", resource->GetName(), key);

		GetRagePageFlagsExtension ext;
		ext.fileName = fileName.c_str();

		auto device = vfs::GetDevice(fileName);

		uint32_t rscPagesVirtual = device->GetLength(fileName);
		uint32_t rscPagesPhysical = 0;

		if (device->ExtensionCtl(VFS_GET_RAGE_PAGE_FLAGS, &ext, sizeof(ext)))
		{
			rscPagesVirtual = ext.flags.flag1;
			rscPagesPhysical = ext.flags.flag2;
		}

		rage::ResourceFlags flags{ rscPagesVirtual, rscPagesPhysical };
		CfxCollection_AddStreamingFileByTag(resource->GetName(), fileName, flags);

		g_readyStreamFiles.insert(fileName);
	});

	fx::ScriptEngine::RegisterNativeHandler("REGISTER_STREAMING_FILE_FROM_URL", [](fx::ScriptContext& context)
	{
		std::string fileName = context.CheckArgument<const char*>(0);
		std::string sourceUrl = context.CheckArgument<const char*>(1);

		fx::OMPtr<IScriptRuntime> runtime;

		if (FX_FAILED(fx::GetCurrentScriptRuntime(&runtime)))
		{
			throw std::runtime_error("no current script runtime");
		}

		// anonymize query string parameters in case these are used for anything malign
		try
		{
			auto uri = skyr::make_url(sourceUrl);
			
			if (uri)
			{
				if (!uri->search().empty())
				{
					uri->set_search(fmt::sprintf("hash=%08x", HashString(uri->search().c_str())));
					sourceUrl = uri->href();
				}
			}
			else
			{
				throw std::runtime_error("invalid streaming URL");
			}
		}
		catch (std::exception& e)
		{
			throw std::runtime_error(va("invalid streaming URL: %s", e.what()));
		}

		auto resource = reinterpret_cast<fx::Resource*>(runtime->GetParentObject());

		auto headerList = std::make_shared<HttpHeaderList>();

		HttpRequestOptions options;
		options.headers["range"] = "bytes=0-15";
		options.responseHeaders = headerList;

		Instance<HttpClient>::Get()->DoGetRequest(sourceUrl, options, [fileName, sourceUrl, resource, headerList](bool success, const char* data, size_t size)
		{
			if (success)
			{
				struct
				{
					uint32_t magic;
					uint32_t version;
					uint32_t virtPages;
					uint32_t physPages;
				} rsc7Header;

				memcpy(&rsc7Header, data, sizeof(rsc7Header));

				auto it = headerList->find("content-range");
				size_t length = 0;

				if (it == headerList->end())
				{
					it = headerList->find("content-length");

					if (it == headerList->end())
					{
						trace("Invalid HTTP response from %s.\n", sourceUrl);
						return;
					}

					length = atoi(it->second.c_str());
				}
				else
				{
					if (it->second.length() >= 6 && it->second.find("bytes ") == 0)
					{
						length = atoi(it->second.substr(it->second.find_last_of("/") + 1).c_str());
					}
				}

				bool isResource = false;
				uint32_t rscVersion = 0;
				uint32_t rscPagesPhysical = 0;
				uint32_t rscPagesVirtual = length;

				if (rsc7Header.magic == 0x37435352) // RSC7
				{
					rscVersion = rsc7Header.version;
					rscPagesPhysical = rsc7Header.physPages;
					rscPagesVirtual = rsc7Header.virtPages;
					isResource = true;
				}

				// TODO: marshal this to the main thread
				ResourceCacheEntryList::Entry rclEntry;
				rclEntry.basename = fileName;
				rclEntry.referenceHash = ""; // empty reference hash should trigger RCD to use the source URL as key
				rclEntry.remoteUrl = sourceUrl;
				rclEntry.resourceName = resource->GetName();
				rclEntry.size = length;
				rclEntry.extData = {
					{ "rscVersion", std::to_string(rscVersion) },
					{ "rscPagesPhysical", std::to_string(rscPagesPhysical) },
					{ "rscPagesVirtual", std::to_string(rscPagesVirtual) },
				};

				resource->GetComponent<ResourceCacheEntryList>()->AddEntry(rclEntry);

				rage::ResourceFlags flags{ rscPagesVirtual, rscPagesPhysical };

				CfxCollection_AddStreamingFileByTag(resource->GetName(), fmt::sprintf("cache:/%s/%s", resource->GetName(), fileName), flags);

				g_readyStreamFiles.insert(fileName);
			}
		});
	});
#endif
});
