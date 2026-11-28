// ring_sink.cpp - the ring sink (issue 3302, epic 3300): producer side of shell/common/vp_frame.h, x64.
// A ring of shared textures (colour B8G8R8A8_UNORM + depth R32_FLOAT per slot, NT handles by name, no keyed mutex),
// the slot rule (Dekker handshake with the composite's slot claim), and either a shared fence (VPSYNC_FENCE: Signal +
// Flush, READY at submit) or CPU sync (VPSYNC_CPU: READY after the GPU finished). Never blocks except the CPU mode's
// GPU wait in end(), which is bounded (2 s, then Error).
//
// Atomicity: the x64 side's aligned 64-bit loads and stores are single accesses. The x86 consumer uses interlocked
// 64-bit operations for the fields it writes outside a seqlock (ring_source.cpp).
#include "ring_sink.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>

namespace vpframe {

using ring::Qpc;
using ring::SafeRelease;

static const uint32_t kMaxDim = 16384;
static const uint32_t kDepthNdcZ = 1;          // shell_shared.h VPDEPTH_NDC_Z
static const double kCpuWaitLimitMs = 2000.0;  // CPU mode: a GPU that does not finish a frame in 2 s is an error

RingSink::RingSink() {}
RingSink::~RingSink() { release(); }

static inline void SeqBegin(volatile uint32_t* seq) { *seq = *seq | 1u; MemoryBarrier(); }
static inline void SeqEnd(volatile uint32_t* seq) { MemoryBarrier(); *seq = *seq + 1u; }

bool RingSink::open(ID3D11Device* dev, ID3D11DeviceContext* ctx, const SinkConfig& cfg)
{
	if (m_sec) release();
	if (!dev || !ctx || !cfg.baseName || !*cfg.baseName || wcslen(cfg.baseName) >= _countof(m_base) - 32)
	{
		m_log("ring sink: open refused: no device, context or base name (or the name is too long)");
		return false;
	}
	m_dev = dev; m_ctx = ctx; m_cfg = cfg;
	wcscpy_s(m_base, cfg.baseName);
	m_cfg.baseName = m_base;

	void* sec = ring::MapSection(m_base, m_map);
	if (!sec) { m_log("ring sink: section %ls%ls not mapped (%lu)", m_base, VPFRAME_SECTION_SUFFIX, GetLastError()); return false; }
	VPFrameServer* srv = VPFRAME_SERVER(sec);
	if (srv->magic == VPFRAME_MAGIC && srv->serverPid != GetCurrentProcessId() && srv->state != VPFS_STOPPED)
	{
		// Another server may still be alive on this section: refuse rather than take over its slots.
		HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, srv->serverPid);
		bool alive = hp && WaitForSingleObject(hp, 0) == WAIT_TIMEOUT;
		if (hp) CloseHandle(hp);
		if (alive)
		{
			m_log("ring sink: section %ls%ls is served by live pid %u: refused", m_base, VPFRAME_SECTION_SUFFIX, srv->serverPid);
			UnmapViewOfFile(sec); CloseHandle(m_map); m_map = nullptr;
			return false;
		}
		m_log("ring sink: taking over the section from dead pid %u (generation %u)", srv->serverPid, VPFRAME_RING(sec)->generation);
	}
	m_sec = sec;
	m_cam = ring::OpenEvent_(m_base, VPFRAME_CAM_EVENT_SUFFIX);
	m_frm = ring::OpenEvent_(m_base, VPFRAME_FRM_EVENT_SUFFIX);
	if (!m_cam || !m_frm) { m_log("ring sink: events not created (%lu)", GetLastError()); release(); return false; }

	// Fence (VPSYNC_FENCE): only when the device answers ID3D11Device5 / ID3D11DeviceContext4 and a shared fence can be made.
	// A new fence per generation (newFence): when a process that opened a shared fence ends, Windows completes that
	// fence to UINT64_MAX [L 2026-11-13, research renderer-in-server-design.md 4.4], so a fence never outlives its consumer.
	if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device5), (void**)&m_dev5)) &&
		SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&m_ctx4)))
	{
		if (!newFence()) m_log("ring sink: shared fence unavailable: CPU sync only");
	}
	else m_log("ring sink: no ID3D11Device5/ID3D11DeviceContext4: CPU sync only");
	D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
	if (FAILED(dev->CreateQuery(&qd, &m_doneQuery))) { m_log("ring sink: CreateQuery failed"); release(); return false; }

	// Initialise the server and ring blocks (we are their only writer from now on); magic last.
	srv->magic = 0;
	MemoryBarrier();
	VPFrameRing* rg = VPFRAME_RING(sec);
	if (rg->generation > m_gen) m_gen = rg->generation;   // names stay unique across restarts and reopenings
	srv->version = VPFRAME_VERSION;
	srv->serverPid = GetCurrentProcessId();
	srv->state = VPFS_STARTING;
	srv->failReason = VPFSF_NONE;
	srv->qpcFrequency = ring::QpcFreq();
	srv->heartbeatQpc = Qpc();
	srv->lastDrawnFrameId = 0;
	srv->drawsTotal = srv->camerasSkipped = srv->skippedBusyTotal = srv->drawErrorsTotal = 0;
	SeqBegin(&rg->seq);
	rg->slotCount = 0; rg->width = rg->height = 0; rg->sync = VPSYNC_NONE;
	rg->srvFenceHandleInExe = 0; rg->exePidForHandle = 0;
	rg->adapterLuid = cfg.adapterLuid;
	SeqEnd(&rg->seq);
	rg->latestSlot = -1;
	rg->latestFrameId = 0;
	resetSlotRecords();
	m_haveRing = false;
	m_ackReqSeq = 0;
	m_latestSlot = -1;
	m_begun = -1;
	m_regenFailQpc = 0;
	srv->state = VPFS_READY;
	MemoryBarrier();
	srv->magic = VPFRAME_MAGIC;
	m_log("ring sink: open on %ls (pid %u, fence %s, generation base %u)", m_base, srv->serverPid, m_fence ? "yes" : "no", m_gen);
	return true;
}

uint64_t RingSink::fenceCompleted() const { return m_fence ? m_fence->GetCompletedValue() : 0; }

int RingSink::freeSlots() const
{
	if (!m_sec || !m_haveRing) return 0;
	const VPFramePublish* pub = VPFRAME_PUBLISH(m_sec);
	int n = 0;
	for (uint32_t s = 0; s < m_slotCount; ++s)
		if ((int)s != m_latestSlot && pub->slotInUse[s] <= pub->slotReleased[s]) ++n;
	return n;
}

bool RingSink::newFence()
{
	if (m_fenceShared) { CloseHandle(m_fenceShared); m_fenceShared = nullptr; }
	SafeRelease(m_fence);
	m_fenceValue = 0;
	if (!m_dev5 || !m_ctx4) return false;
	HRESULT hr = m_dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&m_fence);
	if (SUCCEEDED(hr)) hr = m_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &m_fenceShared);
	if (FAILED(hr))
	{
		m_log("ring sink: CreateFence(SHARED)/CreateSharedHandle failed 0x%08lX", hr);
		SafeRelease(m_fence); m_fenceShared = nullptr;
		return false;
	}
	return true;
}

void RingSink::setState(uint32_t state, uint32_t failReason)
{
	if (!m_sec) return;
	VPFrameServer* srv = VPFRAME_SERVER(m_sec);
	srv->failReason = failReason;
	MemoryBarrier();
	srv->state = state;
}

void RingSink::resetSlotRecords()
{
	for (uint32_t s = 0; s < VPFRAME_MAX_SLOTS; ++s)
	{
		VPFrameSlot* r = VPFRAME_SLOT(m_sec, s);
		SeqBegin(&r->seq);
		r->state = VPSLOT_EMPTY;
		r->frameId = 0;
		r->fenceValue = 0;
		SeqEnd(&r->seq);
	}
}

void RingSink::releaseTextures()
{
	for (auto& s : m_slots)
	{
		SafeRelease(s.rtv); SafeRelease(s.colour); SafeRelease(s.depth);
		if (s.hColour) { CloseHandle(s.hColour); s.hColour = nullptr; }
		if (s.hDepth) { CloseHandle(s.hDepth); s.hDepth = nullptr; }
	}
	SafeRelease(m_dsv); SafeRelease(m_privDepth);
	m_haveRing = false;
	m_slotCount = 0;
}

static bool MakeShared(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& td, const wchar_t* name, ID3D11Texture2D*& tex, HANDLE& h,
	const ring::Logger& log)
{
	HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
	if (FAILED(hr)) { log("ring sink: CreateTexture2D(%ls) failed 0x%08lX", name, hr); return false; }
	IDXGIResource1* r1 = nullptr;
	hr = tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1);
	if (SUCCEEDED(hr)) hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &h);
	SafeRelease(r1);
	if (FAILED(hr)) { log("ring sink: CreateSharedHandle(%ls) failed 0x%08lX", name, hr); h = nullptr; return false; }
	return true;
}

bool RingSink::regenerate(uint32_t slots, uint32_t w, uint32_t h, uint32_t sync, uint32_t exePid, uint32_t reqSeq)
{
	VPFrameRing* rg = VPFRAME_RING(m_sec);
	// Answer "no ring" first, so the consumer never pairs the old generation's names with new slot records.
	SeqBegin(&rg->seq);
	rg->slotCount = 0;
	SeqEnd(&rg->seq);
	rg->latestSlot = -1;
	rg->latestFrameId = 0;
	m_latestSlot = -1;
	releaseTextures();
	resetSlotRecords();
	m_ackReqSeq = reqSeq;

	if (w == 0 || h == 0 || w > kMaxDim || h > kMaxDim)
	{
		m_log("ring sink: request %ux%u refused (1..%u)", w, h, kMaxDim);
		return false;
	}
	if (slots < VPFRAME_MIN_SLOTS || slots > VPFRAME_MAX_SLOTS)
	{
		uint32_t c = slots < VPFRAME_MIN_SLOTS ? VPFRAME_MIN_SLOTS : VPFRAME_MAX_SLOTS;
		m_log("ring sink: %u slots requested, clamped to %u", slots, c);
		slots = c;
	}
	uint32_t useSync = (sync == VPSYNC_CPU) ? VPSYNC_CPU : VPSYNC_FENCE;
	uint32_t handleInExe = 0;
	if (useSync == VPSYNC_FENCE)
	{
		if (!exePid || !newFence()) useSync = VPSYNC_CPU;
		else
		{
			HANDLE hp = OpenProcess(PROCESS_DUP_HANDLE, FALSE, exePid);
			HANDLE dup = nullptr;
			if (hp && DuplicateHandle(GetCurrentProcess(), m_fenceShared, hp, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
				handleInExe = (uint32_t)(uintptr_t)dup;
			else
			{
				m_log("ring sink: fence handle not duplicated into pid %u (%lu): CPU sync for this generation", exePid, GetLastError());
				useSync = VPSYNC_CPU;
			}
			if (hp) CloseHandle(hp);
		}
	}

	uint32_t gen = ++m_gen;
	uint32_t pid = GetCurrentProcessId();
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	bool ok = true;
	for (uint32_t s = 0; s < slots && ok; ++s)
	{
		wchar_t n[256];
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		ok = ring::TexName(n, 256, m_base, pid, gen, s, L'C') && MakeShared(m_dev, td, n, m_slots[s].colour, m_slots[s].hColour, m_log);
		if (ok) ok = SUCCEEDED(m_dev->CreateRenderTargetView(m_slots[s].colour, nullptr, &m_slots[s].rtv));
		td.Format = DXGI_FORMAT_R32_FLOAT;
		// also a render target: end()'s CopyResource from the D32 depth is a draw in DXVK (Wine, Proton), which needs a
		// colour attachment, and DXVK cannot add that usage to a shared image later (the copy then does nothing)
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if (ok) ok = ring::TexName(n, 256, m_base, pid, gen, s, L'D') && MakeShared(m_dev, td, n, m_slots[s].depth, m_slots[s].hDepth, m_log);
	}
	if (ok)
	{
		D3D11_TEXTURE2D_DESC dd = {};
		dd.Width = w; dd.Height = h; dd.MipLevels = 1; dd.ArraySize = 1; dd.SampleDesc.Count = 1;
		// a shader resource too: the source of that copy, sampled by DXVK's draw (without it DXVK makes the image again)
		dd.Format = DXGI_FORMAT_R32_TYPELESS; dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		ok = SUCCEEDED(m_dev->CreateTexture2D(&dd, nullptr, &m_privDepth));
		D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
		dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		if (ok) ok = SUCCEEDED(m_dev->CreateDepthStencilView(m_privDepth, &dv, &m_dsv));
		if (!ok) m_log("ring sink: private depth buffer %ux%u not created", w, h);
	}
	if (!ok)
	{
		releaseTextures();
		if (handleInExe)
		{
			// take the duplicated handle back out of the EXE (it was never published)
			HANDLE hp = OpenProcess(PROCESS_DUP_HANDLE, FALSE, exePid);
			if (hp) { DuplicateHandle(hp, (HANDLE)(uintptr_t)handleInExe, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE); CloseHandle(hp); }
		}
		if (deviceLost()) m_log("ring sink: device removed while creating generation %u", gen);
		return false;
	}
	m_slotCount = slots; m_w = w; m_h = h; m_sync = useSync;
	SeqBegin(&rg->seq);
	rg->generation = gen;
	rg->slotCount = slots;
	rg->width = w; rg->height = h;
	rg->colourFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
	rg->depthFormat = DXGI_FORMAT_R32_FLOAT;
	rg->depthEncoding = kDepthNdcZ;
	rg->sync = useSync;
	rg->ackReqSeq = reqSeq;
	rg->adapterLuid = m_cfg.adapterLuid;
	rg->srvFenceHandleInExe = handleInExe;
	rg->exePidForHandle = handleInExe ? exePid : 0;
	// the textures' NT handles in this process, kept open until releaseTextures(): the composite duplicates them from here
	for (uint32_t s = 0; s < VPFRAME_MAX_SLOTS; ++s)
	{
		rg->slotHandle[s][0] = s < slots ? (uint32_t)(uintptr_t)m_slots[s].hColour : 0;
		rg->slotHandle[s][1] = s < slots ? (uint32_t)(uintptr_t)m_slots[s].hDepth : 0;
	}
	SeqEnd(&rg->seq);
	m_haveRing = true;
	m_log("ring sink: generation %u: %u slots %ux%u sync %s (fence handle 0x%x in pid %u), answers request %u", gen, slots, w, h,
		useSync == VPSYNC_FENCE ? "fence" : "cpu", handleInExe, handleInExe ? exePid : 0, reqSeq);
	return true;
}

void RingSink::tick(int64_t nowQpc)
{
	if (!m_sec) return;
	VPFrameServer* srv = VPFRAME_SERVER(m_sec);
	srv->heartbeatQpc = nowQpc ? nowQpc : Qpc();
	if (m_begun >= 0) return;   // never regenerate under a begun frame
	VPFramePublish* pub = VPFRAME_PUBLISH(m_sec);
	if (m_haveRing && m_sync == VPSYNC_FENCE && m_fence)
	{
		uint64_t done = m_fence->GetCompletedValue();
		if (done > m_fenceValue)
		{
			// Completed beyond what we signalled: a process that opened the fence ended (Windows completes it to
			// UINT64_MAX), so it no longer orders anything. Same request, new generation, new fence.
			m_log("ring sink: fence completed 0x%llX beyond the signalled %llu (its consumer ended): new generation", done, m_fenceValue);
			if (!regenerate(m_slotCount, m_w, m_h, VPSYNC_FENCE, pub->exePid, m_ackReqSeq)) srv->drawErrorsTotal++;
			return;
		}
	}
	uint32_t rs = pub->reqSeq;
	MemoryBarrier();
	uint32_t slots = pub->reqSlots, w = pub->reqWidth, h = pub->reqHeight, sync = pub->reqSync, exePid = pub->exePid;
	MemoryBarrier();
	if (pub->reqSeq != rs || rs == 0) return;      // being written, or no request yet
	if (rs == m_ackReqSeq) return;                // answered (a failed answer is retried only on a new request)
	if (!slots) slots = m_cfg.slots;
	if (!sync) sync = m_cfg.sync;
	if (!regenerate(slots, w, h, sync, exePid, rs))
		srv->drawErrorsTotal++;
}

SinkStatus RingSink::begin(const FrameInfo& f, FrameTargets& out)
{
	(void)f;
	out = FrameTargets();
	if (!m_sec || !m_haveRing) return SinkStatus::NotReady;
	if (m_begun >= 0) abandon();
	VPFramePublish* pub = VPFRAME_PUBLISH(m_sec);
	// Candidates: every slot but the newest READY one, oldest frame first (EMPTY = 0 first).
	int order[VPFRAME_MAX_SLOTS]; int n = 0;
	for (uint32_t s = 0; s < m_slotCount; ++s) if ((int)s != m_latestSlot) order[n++] = (int)s;
	for (int i = 1; i < n; ++i)
		for (int j = i; j > 0 && VPFRAME_SLOT(m_sec, order[j])->frameId < VPFRAME_SLOT(m_sec, order[j - 1])->frameId; --j)
		{ int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
	for (int i = 0; i < n; ++i)
	{
		int s = order[i];
		VPFrameSlot* r = VPFRAME_SLOT(m_sec, s);
		uint32_t oldState = r->state;
		SeqBegin(&r->seq);               // odd: the slot claim of the composite now fails
		r->state = VPSLOT_DRAWING;
		MemoryBarrier();                  // Dekker: our store above before our loads below
		int64_t inUse = pub->slotInUse[s], released = pub->slotReleased[s];
		if (inUse > released)
		{
			r->state = oldState;          // the composite holds it, or its GPU still reads it
			SeqEnd(&r->seq);
			continue;
		}
		m_begun = s;
		m_beginQpc = Qpc();
		out.colour = m_slots[s].rtv;
		out.depth = m_dsv;
		out.width = m_w; out.height = m_h;
		return SinkStatus::Ok;
	}
	VPFRAME_SERVER(m_sec)->skippedBusyTotal++;
	return SinkStatus::Busy;
}

bool RingSink::deviceLost()
{
	HRESULT r = m_dev ? m_dev->GetDeviceRemovedReason() : S_OK;
	return r == DXGI_ERROR_DEVICE_REMOVED || r == DXGI_ERROR_DEVICE_RESET || r == DXGI_ERROR_DEVICE_HUNG ||
		r == DXGI_ERROR_DRIVER_INTERNAL_ERROR || r == DXGI_ERROR_INVALID_CALL;
}

SinkStatus RingSink::end(const FrameInfo& f, const DrawReport& rep)
{
	if (!m_sec || m_begun < 0) return SinkStatus::Error;
	int s = m_begun;
	VPFrameSlot* r = VPFRAME_SLOT(m_sec, s);
	m_ctx->CopyResource(m_slots[s].depth, m_privDepth);   // D32 -> R32F (one typeless family): the slot's NDC z
	uint64_t fv = 0;
	if (m_sync == VPSYNC_FENCE)
	{
		fv = ++m_fenceValue;
		HRESULT hr = m_ctx4->Signal(m_fence, fv);
		m_ctx->Flush();
		if (FAILED(hr)) { m_log("ring sink: Signal failed 0x%08lX", hr); abandon(); VPFRAME_SERVER(m_sec)->drawErrorsTotal++; return deviceLost() ? SinkStatus::DeviceLost : SinkStatus::Error; }
	}
	int64_t submit = Qpc();
	if (m_sync == VPSYNC_CPU)
	{
		m_ctx->End(m_doneQuery);
		m_ctx->Flush();
		int64_t f0 = Qpc(), fq = ring::QpcFreq();
		for (unsigned spins = 0; !m_testSkipCpuWait; ++spins)
		{
			HRESULT hr = m_ctx->GetData(m_doneQuery, nullptr, 0, 0);
			if (hr == S_OK) break;
			if (FAILED(hr) || (double)(Qpc() - f0) * 1000.0 / fq > kCpuWaitLimitMs)
			{
				m_log("ring sink: GPU wait for frame %lld failed (0x%08lX)", (long long)f.frameId, hr);
				abandon();
				VPFRAME_SERVER(m_sec)->drawErrorsTotal++;
				return deviceLost() ? SinkStatus::DeviceLost : SinkStatus::Error;
			}
			if (spins > 64) SwitchToThread(); else YieldProcessor();
		}
	}
	if (deviceLost()) { abandon(); return SinkStatus::DeviceLost; }
	// The slot record (seq is odd since begin), then latest, then the event.
	r->frameId = f.frameId;
	r->fenceValue = fv;
	r->camPublishQpc = f.publishQpc;
	r->wakeQpc = f.wakeQpc;
	r->submitQpc = submit;
	r->readyQpc = Qpc();
	r->cpuMs = rep.cpuMs;
	r->gpuMs = rep.gpuMs;
	r->drawResult = rep.result;
	r->tilesDrawn = rep.tilesDrawn;
	r->tilesUnbaked = rep.tilesUnbaked >= 0 ? (uint32_t)rep.tilesUnbaked : 0;
	r->unbakedCounted = rep.tilesUnbaked >= 0 ? 1u : 0u;
	r->proj22 = f.proj22; r->proj32 = f.proj32; r->nearZ = f.nearZ; r->farZ = f.farZ;
	r->state = VPSLOT_READY;
	SeqEnd(&r->seq);
	VPFrameRing* rg = VPFRAME_RING(m_sec);
	m_latestSlot = s;
	rg->latestSlot = s;
	MemoryBarrier();
	rg->latestFrameId = f.frameId;
	VPFrameServer* srv = VPFRAME_SERVER(m_sec);
	srv->lastDrawnFrameId = f.frameId;
	srv->drawsTotal++;
	if (rep.result == VPDRAW_ERROR) srv->drawErrorsTotal++;
	m_begun = -1;
	SetEvent(m_frm);
	return SinkStatus::Ok;
}

void RingSink::abandon()
{
	if (!m_sec || m_begun < 0) return;
	// The host may have cleared or drawn into the slot: its old picture is gone, so it becomes EMPTY.
	VPFrameSlot* r = VPFRAME_SLOT(m_sec, m_begun);
	r->state = VPSLOT_EMPTY;
	r->frameId = 0;
	r->fenceValue = 0;
	SeqEnd(&r->seq);
	m_begun = -1;
}

void RingSink::release()
{
	if (m_begun >= 0) abandon();
	releaseTextures();
	if (m_sec)
	{
		VPFrameServer* srv = VPFRAME_SERVER(m_sec);
		if (srv->serverPid == GetCurrentProcessId())
		{
			VPFrameRing* rg = VPFRAME_RING(m_sec);
			SeqBegin(&rg->seq);
			rg->slotCount = 0;
			rg->srvFenceHandleInExe = 0;
			SeqEnd(&rg->seq);
			rg->latestSlot = -1;
			rg->latestFrameId = 0;
			srv->state = VPFS_STOPPED;
			MemoryBarrier();
			srv->magic = 0;
		}
		UnmapViewOfFile(m_sec);
		m_sec = nullptr;
	}
	if (m_map) { CloseHandle(m_map); m_map = nullptr; }
	if (m_cam) { CloseHandle(m_cam); m_cam = nullptr; }
	if (m_frm) { CloseHandle(m_frm); m_frm = nullptr; }
	if (m_fenceShared) { CloseHandle(m_fenceShared); m_fenceShared = nullptr; }
	SafeRelease(m_fence);
	SafeRelease(m_doneQuery);
	SafeRelease(m_ctx4);
	SafeRelease(m_dev5);
	m_dev = nullptr; m_ctx = nullptr;
	m_haveRing = false;
}

}  // namespace vpframe
