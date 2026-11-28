// ring_source.cpp - the ring source (issue 3302, epic 3300): consumer side of shell/common/vp_frame.h, x86 (builds on
// x64 too, for tests). Opens the ring's slot textures by the server's handles (by name without them), claims a slot by the Dekker handshake of vp_frame.h,
// queues ID3D11DeviceContext4::Wait on the server's fence (VPSYNC_FENCE) or nothing (VPSYNC_CPU), samples the slot
// directly (no copy), and gives the slot back when an event query issued after the draw completes (polled, never
// waited on). Never blocks except waitFor(), which ends at its deadline (high-resolution waitable timer + spin).
//
// 64-bit fields shared with the x64 server outside a seqlock (slotInUse, slotReleased, latestFrameId) are read and
// written with interlocked operations: a plain 64-bit access is two 32-bit accesses on x86 and could tear.
#include "ring_source.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include "vp_shared_tex.h"

namespace vpframe {

using ring::Qpc;
using ring::SafeRelease;

static inline int64_t Load64(volatile int64_t* p) { return InterlockedCompareExchange64((volatile LONG64*)p, 0, 0); }
static inline void Store64(volatile int64_t* p, int64_t v) { InterlockedExchange64((volatile LONG64*)p, v); }

enum LogOnce : uint32_t
{
	kLogVersion = 1, kLogSlots = 2, kLogSize = 4, kLogAdapter = 8, kLogNames = 16, kLogFence = 32, kLogPend = 64, kLogFormat = 128,
};

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

RingSource::RingSource(uint32_t slots, uint32_t sync)
	: m_wantSlots(slots < VPFRAME_MIN_SLOTS ? VPFRAME_MIN_SLOTS : slots > VPFRAME_MAX_SLOTS ? VPFRAME_MAX_SLOTS : slots),
	  m_wantSync(sync == VPSYNC_CPU ? VPSYNC_CPU : VPSYNC_FENCE)
{
}

RingSource::~RingSource() { close(); }

bool RingSource::open(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* baseName)
{
	if (m_sec) close();
	if (!dev || !ctx || !baseName || !*baseName || wcslen(baseName) >= _countof(m_base) - 32) return false;
	if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&m_dev1)))
	{
		m_log("ring source: the device has no ID3D11Device1 (OpenSharedResource1): refused");
		return false;
	}
	m_dev = dev; m_ctx = ctx;
	if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device5), (void**)&m_dev5)) ||
		FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&m_ctx4)))
	{
		SafeRelease(m_dev5); SafeRelease(m_ctx4);
		m_log("ring source: no ID3D11Device5/ID3D11DeviceContext4: CPU sync requested");
	}
	if (m_wantSync == VPSYNC_FENCE && ring::UnderWine())
	{
		// Wine (Proton 11 and before) cannot import a fence another process shared: OpenSharedFence ends the EXE inside
		// vkImportSemaphoreWin32HandleKHR ("d3d12 fence from other process"), so this cannot be tried and caught
		m_wantSync = VPSYNC_CPU;
		m_log("ring source: running under Wine: CPU sync requested (Wine cannot open a fence shared by another process)");
	}
	IDXGIDevice* xd = nullptr; IDXGIAdapter* ad = nullptr;
	if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad)))
	{
		DXGI_ADAPTER_DESC d; ad->GetDesc(&d);
		m_luid = ((uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32) | d.AdapterLuid.LowPart;
	}
	SafeRelease(ad); SafeRelease(xd);
	wcscpy_s(m_base, baseName);
	m_sec = ring::MapSection(m_base, m_map);
	m_frm = ring::OpenEvent_(m_base, VPFRAME_FRM_EVENT_SUFFIX);
	m_cam = ring::OpenEvent_(m_base, VPFRAME_CAM_EVENT_SUFFIX);
	if (!m_sec || !m_frm || !m_cam)
	{
		m_log("ring source: section or events of %ls not opened (%lu)", m_base, GetLastError());
		close();
		return false;
	}
	m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!m_timer) m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);   // before Windows 10 1803
	VPFramePublish* pub = VPFRAME_PUBLISH(m_sec);
	// A fresh consumer: nothing of ours is in flight; earlier users of the section (a dead or closed composite) are gone.
	for (uint32_t s = 0; s < VPFRAME_MAX_SLOTS; ++s) { Store64(&pub->slotInUse[s], 0); Store64(&pub->slotReleased[s], 0); m_out[s] = 0; }
	m_claimSeq = 0;
	pub->exePid = GetCurrentProcessId();
	m_owner = GetCurrentThreadId();
	m_syncOverride = 0;
	m_gen = m_genPid = 0; m_failedGen = m_failedPid = 0;
	if (m_reqW && m_reqH) writeRequest();   // a re-open asks for a fresh generation (a fresh fence handle)
	return true;
}

void RingSource::writeRequest()
{
	VPFramePublish* pub = VPFRAME_PUBLISH(m_sec);
	uint32_t sync = m_syncOverride ? m_syncOverride : m_wantSync;
	if (sync == VPSYNC_FENCE && !m_ctx4) sync = VPSYNC_CPU;
	pub->exePid = GetCurrentProcessId();
	pub->reqSlots = m_wantSlots;
	pub->reqWidth = m_reqW;
	pub->reqHeight = m_reqH;
	pub->reqSync = sync;
	MemoryBarrier();
	m_reqSent = (uint32_t)InterlockedIncrement((volatile LONG*)&pub->reqSeq);
	if (!m_reqSent) m_reqSent = (uint32_t)InterlockedIncrement((volatile LONG*)&pub->reqSeq);   // 0 means "none"
}

void RingSource::request(uint32_t width, uint32_t height)
{
	if (width == m_reqW && height == m_reqH && m_reqSent) return;
	m_reqW = width; m_reqH = height;
	if (m_sec && width && height) writeRequest();
}

bool RingSource::serverOk()
{
	if (!m_sec) return false;
	const VPFrameServer* srv = VPFRAME_SERVER(m_sec);
	if (srv->magic != VPFRAME_MAGIC) return false;
	if (srv->version != VPFRAME_VERSION)
	{
		if (!(m_logged & kLogVersion)) { m_logged |= kLogVersion; m_log("ring source: section version %u, expected %u: refused", srv->version, VPFRAME_VERSION); }
		m_stats.refusedSection++;
		return false;
	}
	if (srv->state != VPFS_READY && srv->state != VPFS_STARTING) return false;
	uint32_t pid = srv->serverPid;
	bool gone = false;
	AcquireSRWLockExclusive(&m_lock);
	if (pid != m_srvProcPid)
	{
		if (m_srvProc) CloseHandle(m_srvProc);
		m_srvProc = OpenProcess(SYNCHRONIZE, FALSE, pid);
		m_srvProcPid = pid;
		InterlockedExchange(&m_srvGone, 0);
	}
	if (!m_srvProc || WaitForSingleObject(m_srvProc, 0) != WAIT_TIMEOUT) gone = true;
	ReleaseSRWLockExclusive(&m_lock);
	if (gone) InterlockedExchange(&m_srvGone, 1);
	return !gone;
}

void RingSource::dropRing()
{
	for (uint32_t s = 0; s < VPFRAME_MAX_SLOTS; ++s)
	{
		SafeRelease(m_colSrv[s]); SafeRelease(m_depSrv[s]); SafeRelease(m_colTex[s]); SafeRelease(m_depTex[s]);
	}
	SafeRelease(m_fence);
	m_gen = 0; m_genPid = 0; m_slotCount = 0;
}

bool RingSource::ensureRing()
{
	const VPFrameServer* srv = VPFRAME_SERVER(m_sec);
	VPFrameRing rg; uint32_t seq = 0;
	if (!ring::ReadRing(VPFRAME_RING(m_sec), rg, seq)) return false;
	if (rg.slotCount == 0) return false;   // no ring yet / being recreated
	if (rg.slotCount < VPFRAME_MIN_SLOTS || rg.slotCount > VPFRAME_MAX_SLOTS)
	{
		if (!(m_logged & kLogSlots)) { m_logged |= kLogSlots; m_log("ring source: ring slot count %u outside %u..%u: refused", rg.slotCount, VPFRAME_MIN_SLOTS, VPFRAME_MAX_SLOTS); }
		m_stats.refusedSection++;
		return false;
	}
	if (rg.ackReqSeq != m_reqSent) return false;   // the answer to an older request: the new generation is coming
	if (rg.width != m_reqW || rg.height != m_reqH)
	{
		if (!(m_logged & kLogSize)) { m_logged |= kLogSize; m_log("ring source: ring %ux%u answers request %u for %ux%u: refused", rg.width, rg.height, rg.ackReqSeq, m_reqW, m_reqH); }
		m_stats.refusedSection++;
		return false;
	}
	if (rg.colourFormat != DXGI_FORMAT_B8G8R8A8_UNORM || rg.depthFormat != DXGI_FORMAT_R32_FLOAT ||
		(rg.sync != VPSYNC_FENCE && rg.sync != VPSYNC_CPU))
	{
		if (!(m_logged & kLogFormat)) { m_logged |= kLogFormat; m_log("ring source: ring formats %u/%u sync %u not the contract's: refused", rg.colourFormat, rg.depthFormat, rg.sync); }
		m_stats.refusedSection++;
		return false;
	}
	if (rg.adapterLuid && m_luid && rg.adapterLuid != m_luid)
	{
		if (!(m_logged & kLogAdapter)) { m_logged |= kLogAdapter; m_log("ring source: server adapter %016llX is not ours %016llX: refused", rg.adapterLuid, m_luid); }
		m_stats.refusedSection++;
		return false;
	}
	uint32_t pid = srv->serverPid;
	if (rg.generation == m_gen && pid == m_genPid && seq == m_ringSeq) return true;
	if (rg.generation == m_gen && pid == m_genPid) { m_ringSeq = seq; return true; }   // same generation, re-published
	// A new generation (or a new server): open it.
	if (m_gen && pid != m_genPid) m_stats.serverChanges++;
	dropRing();
	if (rg.generation == m_failedGen && pid == m_failedPid) return false;   // already failed: wait for the next one
	int64_t now = Qpc();
	if (now < m_nextTryQpc) return false;
	bool ok = true;
	if (rg.sync == VPSYNC_FENCE)
	{
		// The fence first: this generation's handle value is valid in this process only once (it is closed after the
		// open), and a generation without a usable fence is useless whatever its textures.
		HRESULT hr = E_FAIL;
		if (m_dev5 && rg.exePidForHandle == GetCurrentProcessId() && rg.srvFenceHandleInExe)
		{
			HANDLE h = (HANDLE)(uintptr_t)rg.srvFenceHandleInExe;
			hr = m_dev5->OpenSharedFence(h, __uuidof(ID3D11Fence), (void**)&m_fence);
			// Closed only after a successful open: an unopened value might name another handle of this process.
			if (SUCCEEDED(hr)) CloseHandle(h);
		}
		if (FAILED(hr))
		{
			m_fence = nullptr; ok = false;
			if (!(m_logged & kLogFence)) { m_logged |= kLogFence; m_log("ring source: fence 0x%x for pid %u not opened (0x%08lX): CPU sync requested", rg.srvFenceHandleInExe, rg.exePidForHandle, hr); }
			m_syncOverride = VPSYNC_CPU;
			writeRequest();
		}
	}
	for (uint32_t s = 0; s < rg.slotCount && ok; ++s)
	{
		wchar_t n[256];
		for (int k = 0; k < 2 && ok; ++k)
		{
			ID3D11Texture2D*& tex = k ? m_depTex[s] : m_colTex[s];
			ok = ring::TexName(n, 256, m_base, pid, rg.generation, s, k ? L'D' : L'C');
			VPSharedOpen how;
			HRESULT hr = ok ? VPOpenSharedTexture(m_dev1, pid, rg.slotHandle[s][k], n, DXGI_SHARED_RESOURCE_READ, &tex, &how) : E_FAIL;
			if (FAILED(hr))
			{
				tex = nullptr; ok = false;
				if (!(m_logged & kLogNames)) { m_logged |= kLogNames; char t[400]; m_log("ring source: %s", VPSharedFailText(t, sizeof(t), how, n, pid, rg.slotHandle[s][k])); }
				break;
			}
			if (s == 0 && k == 0) m_openedVia = how.via;
			D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
			if (td.Width != rg.width || td.Height != rg.height || td.Format != (k ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM))
			{
				ok = false;
				m_log("ring source: %ls is %ux%u format %u, the ring says %ux%u", n, td.Width, td.Height, td.Format, rg.width, rg.height);
				break;
			}
			ID3D11ShaderResourceView*& srv2 = k ? m_depSrv[s] : m_colSrv[s];
			ok = SUCCEEDED(m_dev->CreateShaderResourceView(tex, nullptr, &srv2));
		}
	}
	if (ok && VPFRAME_RING(m_sec)->seq != seq)
	{
		// The server began a new generation while we opened this one: the handle values may name its new textures (or
		// nothing). Not a failure of this generation: drop it and open the next one (which brings its own fence handle).
		dropRing();
		return false;
	}
	if (!ok)
	{
		if (m_fence) writeRequest();   // its fence handle is spent (closed): only a new generation can be opened
		dropRing();
		m_failedGen = rg.generation; m_failedPid = pid;
		m_nextTryQpc = now + ring::QpcFreq() / 4;
		m_stats.openFailures++;
		return false;
	}
	m_gen = rg.generation; m_genPid = pid; m_ringSeq = seq; m_ringSync = rg.sync; m_slotCount = rg.slotCount;
	m_w = rg.width; m_h = rg.height;
	m_stats.generations++;
	m_log("ring source: generation %u of pid %u opened: %u slots %ux%u sync %s, textures %s", m_gen, pid, m_slotCount, m_w, m_h,
		m_ringSync == VPSYNC_FENCE ? "fence" : "cpu", m_openedVia == VPSHARED_HANDLE ? "by handle" : "by name");
	return true;
}

int64_t RingSource::newest()
{
	if (!m_sec || !serverOk()) return 0;
	return Load64(&VPFRAME_RING(m_sec)->latestFrameId);
}

bool RingSource::waitFor(int64_t frameId, int64_t deadlineQpc)
{
	if (!m_sec) return false;
	const int64_t freq = ring::QpcFreq();
	const int64_t spinTicks = freq / 2000;   // the last 0.5 ms are spun: a timer wake can come late on a busy machine
	const bool owner = GetCurrentThreadId() == m_owner;
	const int64_t pollTicks = freq / 4000;   // the slow checks at most every 0.25 ms
	int64_t lastPoll = 0;
	for (;;)
	{
		if (Load64(&VPFRAME_RING(m_sec)->latestFrameId) >= frameId) return true;
		int64_t now = Qpc();
		if (now >= deadlineQpc) return false;
		int64_t left = deadlineQpc - now;
		// Issue 3306 (the bound, frame_source.h): the slow checks - the server's process handle (serverOk) and the give-back
		// of slots (complete(): GetData on the EXE's context) - run at most every 0.25 ms and never inside the last 0.5 ms,
		// as the composite's own loop does since 3304 (bce6889); calling them in every spin made waits end up to 1.1 ms
		// past the deadline on the stand-in (3304, design 3.1).
		if (left > spinTicks && (lastPoll == 0 || now - lastPoll >= pollTicks))
		{
			lastPoll = now;
			if (!serverOk()) return false;   // no server: nothing will come
			// Give back slots whose use by our GPU ended meanwhile: the server may be waiting for one to draw this frame.
			if (owner && m_pendN) complete();
			now = Qpc();
			if (now >= deadlineQpc) return false;
			left = deadlineQpc - now;
		}
		if (left > spinTicks)
		{
			int64_t sleepTicks = left - spinTicks;
			if (sleepTicks > freq / 10) sleepTicks = freq / 10;   // re-check the server at least every 100 ms
			if (owner && m_pendN && sleepTicks > freq / 4000) sleepTicks = freq / 4000;   // poll releases every 0.25 ms
			HANDLE hs[2] = { m_frm, m_timer };
			if (m_timer)
			{
				LARGE_INTEGER due; due.QuadPart = -(sleepTicks * 10000000 / freq);
				if (due.QuadPart > -1) due.QuadPart = -1;
				SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
				WaitForMultipleObjects(2, hs, FALSE, (DWORD)(sleepTicks * 1000 / freq) + 20);
			}
			else
				WaitForSingleObject(m_frm, (DWORD)(sleepTicks * 1000 / freq));
		}
		else
			YieldProcessor();
	}
}

Take RingSource::acquire(ID3D11DeviceContext* ctx, int64_t frameId, SourceFrame& out)
{
	out = SourceFrame();
	if (!m_sec || frameId <= 0 || !serverOk() || !ensureRing()) { m_stats.none++; return Take::None; }
	VPFramePublish* pub = VPFRAME_PUBLISH(m_sec);
	// Candidates: READY slots with a frame <= frameId, newest first (the exact frame, if present, is the newest).
	VPFrameSlot rec[VPFRAME_MAX_SLOTS]; uint32_t seq[VPFRAME_MAX_SLOTS]; int order[VPFRAME_MAX_SLOTS]; int n = 0;
	for (uint32_t s = 0; s < m_slotCount; ++s)
	{
		if (!ring::ReadSlot(VPFRAME_SLOT(m_sec, s), rec[s], seq[s])) continue;
		if (rec[s].state != VPSLOT_READY || rec[s].frameId <= 0 || rec[s].frameId > frameId) continue;
		int j = n++;
		for (; j > 0 && rec[order[j - 1]].frameId < rec[s].frameId; --j) order[j] = order[j - 1];
		order[j] = (int)s;
	}
	for (int i = 0; i < n; ++i)
	{
		int s = order[i];
		// The claim value is a number of ours that grows with every claim, not the slot's frame id: a frame taken again
		// (a miss) after its first use was released must still read as "in use" (slotInUse > slotReleased).
		int64_t prev = Load64(&pub->slotInUse[s]);
		Store64(&pub->slotInUse[s], ++m_claimSeq);   // full barrier (xchg)
		VPFrameSlot again; uint32_t seq2 = 0; VPFrameRing rg; uint32_t rseq = 0;
		bool ok = ring::ReadSlot(VPFRAME_SLOT(m_sec, s), again, seq2) && seq2 == seq[s] && again.state == VPSLOT_READY &&
			again.frameId == rec[s].frameId && ring::ReadRing(VPFRAME_RING(m_sec), rg, rseq) && rseq == m_ringSeq;
		if (!ok)
		{
			Store64(&pub->slotInUse[s], prev);   // give it back exactly as it was (a pending older use stays counted)
			m_stats.claimRetries++;
			continue;
		}
		if (m_ringSync == VPSYNC_FENCE)
		{
			if (m_fence)
			{
				uint64_t done = m_fence->GetCompletedValue();
				if (done == UINT64_MAX)
				{
					// Completed to UINT64_MAX: another process that opened this fence ended; it orders nothing any more.
					// Take nothing from this generation; the server answers with a new one (and we ask for it once).
					Store64(&pub->slotInUse[s], prev);
					if (m_poisonGen != m_gen)
					{
						m_poisonGen = m_gen;
						m_stats.fencePoisoned++;
						m_log("ring source: fence of generation %u completed to UINT64_MAX: not used, new generation requested", m_gen);
						writeRequest();
					}
					m_stats.none++;
					return Take::None;
				}
				if (done >= rec[s].fenceValue) m_stats.fenceDoneAtAcquire++;
				if (done > (uint64_t)m_stats.fenceCompletedMax) m_stats.fenceCompletedMax = (int64_t)done;
			}
			ID3D11DeviceContext4* c4 = m_ctx4;
			bool own = false;
			if (ctx && ctx != m_ctx) { c4 = nullptr; own = SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&c4)); }
			HRESULT hr = (c4 && m_fence) ? c4->Wait(m_fence, rec[s].fenceValue) : E_FAIL;
			if (own) c4->Release();
			if (FAILED(hr))
			{
				Store64(&pub->slotInUse[s], prev);
				if (!(m_logged & kLogFence)) { m_logged |= kLogFence; m_log("ring source: fence Wait failed 0x%08lX", hr); }
				m_stats.none++;
				return Take::None;
			}
			m_stats.fenceWaits++;
		}
		m_out[s]++;
		const VPFrameSlot& r = rec[s];
		out.frameId = r.frameId;
		out.slot = s;
		out.colour = m_colSrv[s];
		out.depth = m_depSrv[s];
		out.proj22 = r.proj22; out.proj32 = r.proj32; out.nearZ = r.nearZ; out.farZ = r.farZ;
		out.camPublishQpc = r.camPublishQpc; out.wakeQpc = r.wakeQpc; out.submitQpc = r.submitQpc; out.readyQpc = r.readyQpc;
		out.serverCpuMs = r.cpuMs; out.serverGpuMs = r.gpuMs;
		out.tilesUnbaked = r.unbakedCounted ? (int32_t)r.tilesUnbaked : -1;
		if (r.frameId == frameId) { m_stats.exact++; return Take::Exact; }
		m_stats.older++;
		return Take::Older;
	}
	m_stats.none++;
	return Take::None;
}

void RingSource::release(ID3D11DeviceContext* ctx, const SourceFrame& f)
{
	if (!m_sec || f.slot < 0 || f.slot >= (int)VPFRAME_MAX_SLOTS || f.frameId <= 0) return;
	if (!ctx) ctx = m_ctx;
	if (m_pendN == kPend) complete();
	if (m_pendN == kPend)
	{
		// Nothing completes (the EXE never flushes?): keep the claim; the server skips the slot (no tearing, no wait).
		if (!(m_logged & kLogPend)) { m_logged |= kLogPend; m_log("ring source: %d slot uses in flight, none complete: slot %d kept", kPend, f.slot); }
		return;
	}
	ID3D11Query* q = m_poolN ? m_pool[--m_poolN] : nullptr;
	if (!q)
	{
		D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
		if (FAILED(m_dev->CreateQuery(&qd, &q))) return;   // keep the claim (as above)
	}
	ctx->End(q);
	Pending& p = m_pend[(m_pendHead + m_pendN) % kPend];
	p.q = q; p.slot = f.slot; p.frameId = f.frameId;
	m_pendN++;
	if (m_pendN > m_stats.pendingMax) m_stats.pendingMax = m_pendN;
}

void RingSource::complete()
{
	VPFramePublish* pub = VPFRAME_PUBLISH(m_sec);
	while (m_pendN)
	{
		Pending& p = m_pend[m_pendHead];
		HRESULT hr = m_ctx->GetData(p.q, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (hr == S_FALSE) break;
		// S_OK, or an error (device removed): either way the GPU no longer reads the slot for this use. The slot is
		// given back only when no other claim of it is open (a held frame, a second take of the same frame).
		if (m_out[p.slot] > 0) m_out[p.slot]--;
		if (m_out[p.slot] == 0) Store64(&pub->slotReleased[p.slot], Load64(&pub->slotInUse[p.slot]));
		if (m_poolN < kPend) m_pool[m_poolN++] = p.q; else p.q->Release();
		p.q = nullptr;
		m_pendHead = (m_pendHead + 1) % kPend;
		m_pendN--;
		m_stats.released++;
	}
}

void RingSource::poll(int64_t nowQpc)
{
	(void)nowQpc;
	if (!m_sec) return;
	if (GetCurrentThreadId() != m_owner) { serverOk(); return; }   // another thread: liveness only, never the context
	complete();
	const VPFrameServer* srv = VPFRAME_SERVER(m_sec);
	if (m_gen && (srv->magic != VPFRAME_MAGIC || srv->serverPid != m_genPid || m_srvGone))
	{
		m_log("ring source: server pid %u gone or replaced (now pid %u, magic %08X): ring dropped", m_genPid, srv->serverPid, srv->magic);
		m_stats.serverChanges++;
		dropRing();
		m_failedGen = m_failedPid = 0;
		// No re-send needed: a new server's open() starts with nothing answered and answers the current reqSeq.
	}
}

void RingSource::close()
{
	if (m_sec && GetCurrentThreadId() == m_owner && m_ctx) complete();
	for (int i = 0; i < m_pendN; ++i) { Pending& p = m_pend[(m_pendHead + i) % kPend]; SafeRelease(p.q); }
	m_pendN = 0; m_pendHead = 0;
	for (int i = 0; i < m_poolN; ++i) SafeRelease(m_pool[i]);
	m_poolN = 0;
	dropRing();
	SafeRelease(m_ctx4); SafeRelease(m_dev5); SafeRelease(m_dev1);
	if (m_sec) { UnmapViewOfFile(m_sec); m_sec = nullptr; }
	if (m_map) { CloseHandle(m_map); m_map = nullptr; }
	if (m_frm) { CloseHandle(m_frm); m_frm = nullptr; }
	if (m_cam) { CloseHandle(m_cam); m_cam = nullptr; }
	if (m_timer) { CloseHandle(m_timer); m_timer = nullptr; }
	AcquireSRWLockExclusive(&m_lock);
	if (m_srvProc) { CloseHandle(m_srvProc); m_srvProc = nullptr; }
	m_srvProcPid = 0;
	ReleaseSRWLockExclusive(&m_lock);
	m_dev = nullptr; m_ctx = nullptr;
	m_reqSent = 0;
}

}  // namespace vpframe
