/*
 * This file is part of the Cfx project - https://cfx.re/
 *
 * See LICENSE in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"

#include <ScriptEngine.h>

#include <CoreConsole.h>
#include <ConsoleHost.h>
#include <InputHook.h>
#include <nutsnbolts.h>

#include <DrawCommands.h>
#include <grcTexture.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include <imgui.h>
#include <im3d.h>
#include <im3d_math.h>

#include "GamePrimitives.h"

// resolved by GraphicsNatives.cpp (same component)
extern CViewportGame** g_viewportGame;

namespace
{
//
// im3d keeps a single global context: geometry is accumulated from the script (main) thread, while the
// render thread may only look at plain vertex data. The draw lists are therefore copied out once per game
// frame and handed over as a snapshot, similar to how the five equivalent does it.
//
// screen space vertex, projected on the script (main) thread and streamed by the render thread
struct ScreenVertex
{
	float m_x;
	float m_y;
	uint32_t m_color;
};

struct RemoteDrawList
{
	int m_drawMode; // 1 = line list, 3 = triangle list
	std::vector<ScreenVertex> m_vertices;
};

std::mutex g_drawListMutex;
std::vector<RemoteDrawList> g_pendingDrawLists; // produced by the main thread
std::vector<RemoteDrawList> g_renderDrawLists;  // consumed by the render thread

bool g_frameOpen;
uint32_t g_frameStartTime;

// key states, toggled by the '+/-gizmo*' console commands (the same names as five, so scripts can
// bind them with RegisterKeyMapping)
bool g_controlState[5];

struct GizmoControl
{
	GizmoControl(const std::string& name, size_t index)
		: m_downCommand("+" + name, [index]() { g_controlState[index] = true; })
		, m_upCommand("-" + name, [index]() { g_controlState[index] = false; })
	{
	}

	ConsoleCommand m_downCommand, m_upCommand;
};

void RegisterControls()
{
	static GizmoControl controls[] = {
		{ "gizmoSelect", 0 }, { "gizmoLocal", 1 }, { "gizmoTranslation", 2 }, { "gizmoRotation", 3 }, { "gizmoScale", 4 } };
}

// defined below, used by CloseFrame
void ProjectDrawList(const Im3d::DrawList& drawList, const DirectX::XMMATRIX& viewProj, int width, int height, float projScaleY, const Im3d::Vec3& viewOrigin, RemoteDrawList& out);

const rage::grcViewport* GetGameViewport()
{
	return (g_viewportGame && *g_viewportGame) ? &(*g_viewportGame)->viewport : nullptr;
}

// m_inverseView is the camera (view -> world) matrix, so it holds the camera position
Im3d::Vec3 GetViewOrigin(const rage::grcViewport& viewport)
{
	return Im3d::Vec3(viewport.m_inverseView[12], viewport.m_inverseView[13], viewport.m_inverseView[14]);
}

// projection[1][1] holds 1 / tan(fovY / 2), im3d expects 2 * tan(fovY / 2)
float GetProjScaleY(const rage::grcViewport& viewport)
{
	return (viewport.m_projection[5] != 0.0f) ? (2.0f / viewport.m_projection[5]) : 1.0f;
}

// world -> clip, both matrices are stored in row major / row vector form
DirectX::XMMATRIX GetViewProj(const rage::grcViewport& viewport)
{
	using namespace DirectX;

	return XMMatrixMultiply(XMLoadFloat4x4((const XMFLOAT4X4*)&viewport.m_worldView), XMLoadFloat4x4((const XMFLOAT4X4*)&viewport.m_projection));
}

// the mouse position is only tracked while a cursor is visible
bool GetCursorRay(const rage::grcViewport& viewport, int width, int height, Im3d::Vec3& origin, Im3d::Vec3& direction)
{
	if (!ImGui::GetCurrentContext())
	{
		return false;
	}

	const ImVec2 mousePos = ImGui::GetIO().MousePos;
	const float x = (mousePos.x - ImGui::GetMainViewport()->Pos.x) / (float)width;
	const float y = (mousePos.y - ImGui::GetMainViewport()->Pos.y) / (float)height;

	if (!width || !height || !std::isfinite(x) || !std::isfinite(y) || x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f)
	{
		return false;
	}

	const rage::Vec3V nearPos = Unproject(viewport, rage::Vec3V{ x, y, 0.0f });
	const rage::Vec3V farPos = Unproject(viewport, rage::Vec3V{ x, y, 1.0f });

	origin = GetViewOrigin(viewport);
	direction = Im3d::Normalize(Im3d::Vec3(farPos.x - nearPos.x, farPos.y - nearPos.y, farPos.z - nearPos.z));

	return std::isfinite(direction.x);
}

void FillAppData()
{
	Im3d::AppData& appData = Im3d::GetAppData();

	const uint32_t now = GetTickCount();
	appData.m_deltaTime = std::min((now - g_frameStartTime) / 1000.0f, 0.1f);
	g_frameStartTime = now;

	int width = 0;
	int height = 0;
	GetGameResolution(width, height);

	appData.m_viewportSize = Im3d::Vec2((float)width, (float)height);
	appData.m_worldUp = Im3d::Vec3(0.0f, 0.0f, 1.0f);
	appData.m_projOrtho = false;
	appData.m_flipGizmoWhenBehind = false;

	appData.m_keyDown[Im3d::Action_Select] = g_controlState[0];
	appData.m_keyDown[Im3d::Action_GizmoLocal] = g_controlState[1];
	appData.m_keyDown[Im3d::Action_GizmoTranslation] = g_controlState[2];
	appData.m_keyDown[Im3d::Action_GizmoRotation] = g_controlState[3];
	appData.m_keyDown[Im3d::Action_GizmoScale] = g_controlState[4];

	const rage::grcViewport* viewport = GetGameViewport();

	if (!viewport || !width || !height)
	{
		return;
	}

	using namespace DirectX;

	XMVECTOR scale, quaternion, translation;
	XMMatrixDecompose(&scale, &quaternion, &translation, XMLoadFloat4x4((const XMFLOAT4X4*)&viewport->m_inverseView));

	const XMVECTOR forward = XMVector3Rotate(XMVectorSet(0.0f, 0.0f, -1.0f, 1.0f), quaternion);

	appData.m_viewOrigin = GetViewOrigin(*viewport);
	appData.m_viewDirection = Im3d::Vec3(XMVectorGetX(forward), XMVectorGetY(forward), XMVectorGetZ(forward));
	appData.m_projScaleY = GetProjScaleY(*viewport);

	// without a cursor the ray is kept outside of the scene, so that no handle can be grabbed
	appData.m_cursorRayOrigin = appData.m_viewOrigin + appData.m_viewDirection * 10000.0f;
	appData.m_cursorRayDirection = appData.m_viewDirection;

	Im3d::Vec3 rayOrigin;
	Im3d::Vec3 rayDirection;

	if (GetCursorRay(*viewport, width, height, rayOrigin, rayDirection))
	{
		appData.m_cursorRayOrigin = rayOrigin;
		appData.m_cursorRayDirection = rayDirection;
	}
}

void CloseFrame()
{
	if (!g_frameOpen)
	{
		return;
	}

	g_frameOpen = false;

	Im3d::EndFrame();

	g_pendingDrawLists.clear();

	int width = 0;
	int height = 0;
	GetGameResolution(width, height);

	const rage::grcViewport* viewport = GetGameViewport();

	if (viewport && width && height)
	{
		const DirectX::XMMATRIX viewProj = GetViewProj(*viewport);
		const Im3d::Vec3 viewOrigin = GetViewOrigin(*viewport);
		const float projScaleY = GetProjScaleY(*viewport);

		for (uint32_t i = 0; i < Im3d::GetDrawListCount(); i++)
		{
			RemoteDrawList drawList;
			ProjectDrawList(Im3d::GetDrawLists()[i], viewProj, width, height, projScaleY, viewOrigin, drawList);

			if (!drawList.m_vertices.empty())
			{
				g_pendingDrawLists.push_back(std::move(drawList));
			}
		}
	}

	std::lock_guard<std::mutex> lock(g_drawListMutex);
	std::swap(g_renderDrawLists, g_pendingDrawLists);
}

uint32_t ConvertColor(Im3d::Color color)
{
	// im3d packs colors as 0xRRGGBBAA, the render system wants them swapped when it swaps colors
	return rage::sga::Texture::IsRenderSystemColorSwapped() ? ((color.v & 0xFF00FF00) | _rotl(color.v & 0x00FF00FF, 16)) : color.v;
}

// projects a draw list into screen space vertices; primitives crossing the camera plane are dropped
void ProjectDrawList(const Im3d::DrawList& drawList, const DirectX::XMMATRIX& viewProj, int width, int height, float projScaleY, const Im3d::Vec3& viewOrigin, RemoteDrawList& out)
{
	using namespace DirectX;

	const size_t verticesPerPrimitive = (drawList.m_primType == Im3d::DrawPrimitive_Triangles) ? 3 : (drawList.m_primType == Im3d::DrawPrimitive_Lines ? 2 : 1);

	out.m_drawMode = (drawList.m_primType == Im3d::DrawPrimitive_Triangles) ? 3 : 1;
	out.m_vertices.clear();

	for (size_t i = 0; i + verticesPerPrimitive <= drawList.m_vertexCount; i += verticesPerPrimitive)
	{
		ScreenVertex primitive[3];
		bool valid = true;

		for (size_t v = 0; valid && v < verticesPerPrimitive; v++)
		{
			const Im3d::VertexData& vertex = drawList.m_vertexData[i + v];
			const XMVECTOR clip = XMVector4Transform(XMVectorSet(vertex.m_positionSize.x, vertex.m_positionSize.y, vertex.m_positionSize.z, 1.0f), viewProj);
			const float w = XMVectorGetW(clip);

			if (!(valid = (w > 0.0001f)))
			{
				break;
			}

			primitive[v] = { (XMVectorGetX(clip) / w * 0.5f + 0.5f) * width, (0.5f - XMVectorGetY(clip) / w * 0.5f) * height, ConvertColor(vertex.m_color) };
		}

		if (!valid)
		{
			continue;
		}

		if (drawList.m_primType == Im3d::DrawPrimitive_Points)
		{
			// the immediate mode vertex has no point size, so points become screen space quads
			const float distance = Im3d::Length(Im3d::Vec3(drawList.m_vertexData[i].m_positionSize) - viewOrigin);
			const float worldSize = drawList.m_vertexData[i].m_positionSize.w;
			const float halfSize = std::min(std::max((distance > 0.0f) ? (worldSize * height / (projScaleY * distance)) * 0.5f : 1.0f, 1.0f), 64.0f);

			const ScreenVertex corners[4] = {
				{ primitive[0].m_x - halfSize, primitive[0].m_y - halfSize, primitive[0].m_color },
				{ primitive[0].m_x + halfSize, primitive[0].m_y - halfSize, primitive[0].m_color },
				{ primitive[0].m_x + halfSize, primitive[0].m_y + halfSize, primitive[0].m_color },
				{ primitive[0].m_x - halfSize, primitive[0].m_y + halfSize, primitive[0].m_color },
			};

			out.m_drawMode = 3;

			for (int index : { 0, 1, 2, 0, 2, 3 })
			{
				out.m_vertices.push_back(corners[index]);
			}

			continue;
		}

		for (size_t v = 0; v < verticesPerPrimitive; v++)
		{
			out.m_vertices.push_back(primitive[v]);
		}
	}
}

void RenderGizmos()
{
	// the five equivalent draws in the scene pass; rdr3 has no such hook, so the gizmo is drawn in the
	// post frontend overlay pass (before nui and the console, which bind with a higher priority)
	if (!IsOnRenderThread())
	{
		return;
	}

	std::vector<RemoteDrawList> drawLists;

	{
		std::lock_guard<std::mutex> lock(g_drawListMutex);

		if (g_renderDrawLists.empty())
		{
			return;
		}

		drawLists.swap(g_renderDrawLists);
	}

	const uint32_t oldRasterizerState = GetRasterizerState();
	const uint32_t oldBlendState = GetBlendState();
	const uint32_t oldDepthStencilState = GetDepthStencilState();

	SetRasterizerState(GetStockStateIdentifier(RasterizerStateNoCulling));
	SetBlendState(GetStockStateIdentifier(BlendStateDefault));
	SetDepthStencilState(GetStockStateIdentifier(DepthStencilStateNoDepth));
	SetScissorRect(0, 0, 0x1FFF, 0x1FFF);

	SetTextureGtaIm(rage::grcTextureFactory::GetNoneTexture());
	PushDrawBlitImShader();

	for (const RemoteDrawList& drawList : drawLists)
	{
		rage::grcBegin(drawList.m_drawMode, (int)drawList.m_vertices.size());

		for (const ScreenVertex& vertex : drawList.m_vertices)
		{
			rage::grcVertex(vertex.m_x, vertex.m_y, 0.0f, 0.0f, 0.0f, -1.0f, vertex.m_color, 0.0f, 0.0f);
		}

		rage::grcEnd();
	}

	PopDrawBlitImShader();

	SetRasterizerState(oldRasterizerState);
	SetBlendState(oldBlendState);
	SetDepthStencilState(oldDepthStencilState);
}
}

static InitFunction initFunction([]()
{
	// create the '+/-gizmo*' console commands at startup, so that key mappings registered by scripts
	// always resolve
	RegisterControls();

	OnMainGameFrame.Connect([]()
	{
		CloseFrame();
	});

	OnPostFrontendRender.Connect([]()
	{
		RenderGizmos();
	}, -2000);

	fx::ScriptEngine::RegisterNativeHandler("DRAW_GIZMO", [](fx::ScriptContext& context)
	{
		float* matrix = context.CheckArgument<float*>(0);
		const char* id = context.CheckArgument<const char*>(1);

		if (!matrix || !id)
		{
			context.SetResult<bool>(false);
			return;
		}

		if (!g_frameOpen)
		{
			g_frameOpen = true;

			Im3d::NewFrame();
			FillAppData();
		}

		const uint32_t uid = Im3d::MakeId(id);

		// workaround for an im3d quirk where a control stays active when the drawn gizmo changes
		static uint32_t lastUid = 0;

		if (uid != lastUid)
		{
			lastUid = uid;
			Im3d::GetContext().resetId();
		}

		context.SetResult<bool>(Im3d::Gizmo(uid, matrix));
	});

	static int cursorModeRefCount = 0;

	fx::ScriptEngine::RegisterNativeHandler("ENTER_CURSOR_MODE", [](fx::ScriptContext& context)
	{
		if (cursorModeRefCount++ == 0)
		{
			ConHost::SetCursorMode(true);
			InputHook::SetControlBypasses(0x3D, { InputHook::ControlBypass{ true, 0 }, InputHook::ControlBypass{ true, 1 }, InputHook::ControlBypass{ true, 2 }, InputHook::ControlBypass{ true, 3 }, InputHook::ControlBypass{ true, 4 } });
		}
	});

	fx::ScriptEngine::RegisterNativeHandler("LEAVE_CURSOR_MODE", [](fx::ScriptContext& context)
	{
		if (cursorModeRefCount-- == 1)
		{
			ConHost::SetCursorMode(false);
			InputHook::SetControlBypasses(0x3D, {});
		}
	});
});
