// srv_fow_mask_pub.cpp - server.exe hands its fog-of-war mask to the EXE's model draws (ticket 0339). Contract:
// srv_fow_mask_pub.h; layout: shell/common/shell_fowmask.h. Written 2026-11-15 for this repository (GPL-3.0-or-later).
#include "srv_fow_mask_pub.h"
#include <dxgi1_2.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace srvrender {

namespace {
template <class T> void Rel(T*& p)
{
	if(p)
	{
		p->Release();
		p = nullptr;
	}
}
const int kMaxLogs = 200;
}  // namespace

const char* FowMaskPublisher::ConsumerStateName(uint32_t s)
{
	static const char* k[] = {"NONE", "WAITING", "ACTIVE", "FAILED", "OFF"};
	return s < 5 ? k[s] : "?";
}

const char* FowMaskPublisher::FailName(uint32_t r)
{
	static const char* k[VPFMCF_COUNT] = {"NONE",      "PATTERN", "WHITE",  "BASE",      "MISMATCH",
	                                      "TEXTURE",   "DEVICE",  "EXCEPTION", "VERSION", "TIMEOUT"};
	return r < VPFMCF_COUNT ? k[r] : "?";
}

void FowMaskPublisher::note(const char* fmt, ...)
{
	if(!m_log || m_logs >= kMaxLogs)
		return;
	++m_logs;
	char b[700];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
	va_end(ap);
	m_log("fog models: %s%s", b, m_logs == kMaxLogs ? " (log line limit reached)" : "");
}

bool FowMaskPublisher::openSection(uint32_t exePid)
{
	if(m_sec && m_exePid == exePid)
		return true;
	if(m_sec)
		release("the EXE's pid changed");
	wchar_t name[96];
	if(!m_nameOverride.empty())
		_snwprintf_s(name, _TRUNCATE, L"%s", m_nameOverride.c_str());
	else
		_snwprintf_s(name, _TRUNCATE, VPSHELL_FOWMASK_SECTION_FMT, exePid);
	m_secH = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, VPSHELL_FOWMASK_SECTION_BYTES, name);
	const DWORD err = GetLastError();
	if(!m_secH)
	{
		faults++;
		note("LOUD: the section '%ls' could not be made: error %lu", name, err);
		return false;
	}
	void* v = MapViewOfFile(m_secH, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, VPSHELL_FOWMASK_SECTION_BYTES);
	if(!v)
	{
		faults++;
		note("LOUD: the section '%ls' could not be mapped: error %lu", name, GetLastError());
		CloseHandle(m_secH);
		m_secH = nullptr;
		return false;
	}
	m_sec = (VPShellFowMaskSection*)v;
	m_exePid = exePid;
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	m_epoch = ((uint32_t)q.QuadPart * 2654435761u) | 1u;
	if(m_epoch == m_sec->epoch)
		m_epoch += 2;
	const bool existed = err == ERROR_ALREADY_EXISTS;
	// a section a consumer still holds from an earlier server keeps its consumer half (wanted, a FAILED state)
	m_sec->magic = 0;
	MemoryBarrier();
	if(!existed)
		memset(m_sec, 0, VPSHELL_FOWMASK_SECTION_BYTES);
	m_sec->version = VPSHELL_FOWMASK_VERSION;
	m_sec->sectionBytes = VPSHELL_FOWMASK_SECTION_BYTES;
	m_sec->slotCount = VPSHELL_FOWMASK_SLOTS;
	m_sec->slotBytes = sizeof(VPShellFowMaskSlot);
	m_sec->exePid = exePid;
	m_sec->serverPid = GetCurrentProcessId();
	m_sec->epoch = m_epoch;
	InterlockedIncrement((volatile LONG*)&m_sec->seq);
	MemoryBarrier();
	m_sec->producerState = VPFMSK_NONE;
	m_sec->newestSlot = VPSHELL_FOWMASK_NONE;
	m_sec->generation = 0;
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&m_sec->seq);
	for(uint32_t i = 0; i < VPSHELL_FOWMASK_SLOTS; ++i)
	{
		VPShellFowMaskSlot* s = VPSHELL_FOWMASK_SLOT(m_sec, i);
		InterlockedIncrement((volatile LONG*)&s->seq);
		MemoryBarrier();
		s->valid = 0;
		MemoryBarrier();
		InterlockedIncrement((volatile LONG*)&s->seq);
	}
	MemoryBarrier();
	m_sec->magic = VPSHELL_FOWMASK_MAGIC;
	m_newest = VPSHELL_FOWMASK_NONE;
	m_gen = 0;
	note("section '%ls' %s (EXE pid %u, epoch %u; consumer %s, wanted %u)", name, existed ? "taken over from an earlier server" : "made",
	     exePid, m_epoch, ConsumerStateName(m_sec->consumerState), m_sec->wanted);
	return true;
}

bool FowMaskPublisher::makeSlots(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& src)
{
	dropSlots();
	for(uint32_t i = 0; i < VPSHELL_FOWMASK_SLOTS; ++i)
	{
		Slot& s = m_slot[i];
		D3D11_TEXTURE2D_DESC n = src;
		n.Usage = D3D11_USAGE_DEFAULT;
		n.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		n.CPUAccessFlags = 0;
		n.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		n.ArraySize = 1;
		n.SampleDesc.Count = 1;
		n.SampleDesc.Quality = 0;
		HRESULT hr = dev->CreateTexture2D(&n, nullptr, &s.tex);
		if(FAILED(hr))
		{
			note("LOUD: slot %u: CreateTexture2D %ux%u format %u mips %u failed 0x%08X", i, n.Width, n.Height, (unsigned)n.Format,
			     n.MipLevels, (unsigned)hr);
			s.tex = nullptr;
			dropSlots();
			return false;
		}
		wchar_t name[96];
		_snwprintf_s(name, _TRUNCATE, VPSHELL_FOWMASK_TEXTURE_FMT, m_exePid, m_epoch, i);
		IDXGIResource1* r = nullptr;
		hr = s.tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&r);
		if(SUCCEEDED(hr))
		{
			hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, name, &s.nt);
			r->Release();
		}
		D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
		if(SUCCEEDED(hr))
			hr = dev->CreateQuery(&qd, &s.q);
		if(FAILED(hr))
		{
			note("LOUD: slot %u: shared handle '%ls' or its query not made: 0x%08X", i, name, (unsigned)hr);
			s.nt = nullptr;
			dropSlots();
			return false;
		}
		VPShellFowMaskSlot* m = VPSHELL_FOWMASK_SLOT(m_sec, i);
		InterlockedIncrement((volatile LONG*)&m->seq);
		MemoryBarrier();
		m->valid = 0;
		m->texEpoch = m_epoch;
		m->ntHandle = (uint32_t)(uintptr_t)s.nt;   // the consumer duplicates it from this process (vp_shared_tex.h)
		m->width = src.Width;
		m->height = src.Height;
		m->mips = src.MipLevels;
		m->dxgiFormat = (uint32_t)src.Format;
		MemoryBarrier();
		InterlockedIncrement((volatile LONG*)&m->seq);
	}
	note("%u slots of %ux%u, %u mips, format %u made (NT-shared, epoch %u), %.1f MB of video memory", VPSHELL_FOWMASK_SLOTS, src.Width,
	     src.Height, src.MipLevels, (unsigned)src.Format, m_epoch,
	     VPSHELL_FOWMASK_SLOTS * (double)src.Width * src.Height * 4.0 * 4.0 / 3.0 / 1048576.0);
	return true;
}

void FowMaskPublisher::dropSlots()
{
	if(m_sec)
	{
		InterlockedIncrement((volatile LONG*)&m_sec->seq);
		MemoryBarrier();
		m_sec->newestSlot = VPSHELL_FOWMASK_NONE;
		MemoryBarrier();
		InterlockedIncrement((volatile LONG*)&m_sec->seq);
		for(uint32_t i = 0; i < VPSHELL_FOWMASK_SLOTS; ++i)
		{
			VPShellFowMaskSlot* m = VPSHELL_FOWMASK_SLOT(m_sec, i);
			InterlockedIncrement((volatile LONG*)&m->seq);
			MemoryBarrier();
			m->valid = 0;
			m->ntHandle = 0;   // closed below
			MemoryBarrier();
			InterlockedIncrement((volatile LONG*)&m->seq);
		}
	}
	for(Slot& s : m_slot)
	{
		Rel(s.q);
		if(s.nt)
			CloseHandle(s.nt);
		s.nt = nullptr;
		Rel(s.tex);
		s.inflight = false;
	}
	m_inflight = -1;
	m_newest = VPSHELL_FOWMASK_NONE;
	// a new epoch for the next textures: a consumer re-opens them
	m_epoch += 2;
	if(m_sec)
		m_sec->epoch = m_epoch;
}

void FowMaskPublisher::pollInflight(ID3D11DeviceContext* ctx)
{
	if(m_inflight < 0)
		return;
	Slot& s = m_slot[m_inflight];
	// without a flush the first frames after the copy; then with one (WARP and an idle queue may not start the copy
	// otherwise: 2026-11-15, the stand-in tests saw a query never signal with DONOTFLUSH alone)
	const HRESULT hr = ctx->GetData(s.q, nullptr, 0, ++m_polls > 2 ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
	if(hr != S_OK)
	{
		if(m_polls == 3 || m_polls == 30)
			note("the copy into slot %d has not signalled after %d polls (GetData 0x%08X)", m_inflight, m_polls, (unsigned)hr);
		return;
	}
	m_polls = 0;
	const uint32_t i = (uint32_t)m_inflight;
	s.inflight = false;
	m_inflight = -1;
	VPShellFowMaskSlot* m = VPSHELL_FOWMASK_SLOT(m_sec, i);
	InterlockedIncrement((volatile LONG*)&m->seq);
	MemoryBarrier();
	m->valid = 1;
	m->generation = ++m_gen;
	m->texEpoch = m_epoch;
	m->ntHandle = (uint32_t)(uintptr_t)s.nt;
	m->numViews = s.meta.numViews;
	m->pairGeneration = s.meta.pairGeneration;
	m->pairPresentCount = s.meta.pairPresentCount;
	m->fogGeneration = s.meta.fogGeneration;
	m->builtQpc = s.qpc;
	memcpy(m->fowAnim, s.meta.fowAnim, sizeof(m->fowAnim));
	memcpy(m->overlayProj, s.meta.overlayProj, sizeof(m->overlayProj));
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&m->seq);
	InterlockedIncrement((volatile LONG*)&m_sec->seq);
	MemoryBarrier();
	m_sec->producerState = VPFMSK_ACTIVE;
	m_sec->newestSlot = i;
	m_sec->generation = m_gen;
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&m_sec->seq);
	m_newest = i;
	published++;
	InterlockedExchange((volatile LONG*)&m_sec->publishedTotal, (LONG)published);
	if(published == 1)
		note("first mask handed over (slot %u, generation %u, pair generation %u, %u views)", i, m_gen, s.meta.pairGeneration,
		     s.meta.numViews);
}

void FowMaskPublisher::frame(ID3D11DeviceContext* ctx, uint32_t exePid, ID3D11Texture2D* mask, bool newMask, const FowMaskPubInput& in)
{
	if(!ctx || exePid == 0)
		return;
	if(!openSection(exePid))
		return;
	InterlockedIncrement((volatile LONG*)&m_sec->heartbeat);
	const uint32_t cs = m_sec->consumerState;
	if(cs != m_lastConsumer)
	{
		note("consumer %s (wanted %u, pid %u)", ConsumerStateName(cs), m_sec->wanted, m_sec->consumerPid);
		m_lastConsumer = cs;
	}
	if(!m_sec->wanted || cs == VPFMC_FAILED || cs == VPFMC_OFF)
	{
		if(m_slot[0].tex)
		{
			note("the consumer does not want the mask (%s): slot textures released", ConsumerStateName(cs));
			dropSlots();
			m_sec->producerState = VPFMSK_NONE;
		}
		m_pending = false;
		return;
	}
	pollInflight(ctx);
	if(!mask)
		return;
	D3D11_TEXTURE2D_DESC d;
	mask->GetDesc(&d);
	if(!m_slot[0].tex)
	{
		if(m_slotsFailed && m_haveDesc && d.Width == m_desc.Width && d.MipLevels == m_desc.MipLevels && d.Format == m_desc.Format)
			return;
		ID3D11Device* dev = nullptr;
		ctx->GetDevice(&dev);
		const bool ok = dev && makeSlots(dev, d);
		Rel(dev);
		m_desc = d;
		m_haveDesc = true;
		m_slotsFailed = !ok;
		if(!ok)
		{
			faults++;
			InterlockedExchange((volatile LONG*)&m_sec->faultsTotal, (LONG)faults);
			return;
		}
		newMask = true;
	}
	else if(d.Width != m_desc.Width || d.Height != m_desc.Height || d.MipLevels != m_desc.MipLevels || d.Format != m_desc.Format)
	{
		note("the mask changed its description (%ux%u, %u mips -> %ux%u, %u mips): slots made again", m_desc.Width, m_desc.Height,
		     m_desc.MipLevels, d.Width, d.Height, d.MipLevels);
		dropSlots();
		m_slotsFailed = false;
		frame(ctx, exePid, mask, true, in);
		return;
	}
	if(!newMask && !m_pending && m_newest != VPSHELL_FOWMASK_NONE)
		return;
	if(m_inflight >= 0)
	{
		// the previous copy has not signalled yet: this mask goes at the next frame (the newer one then)
		m_pending = true;
		skipped++;
		InterlockedExchange((volatile LONG*)&m_sec->skippedTotal, (LONG)skipped);
		return;
	}
	for(uint32_t i = 0; i < VPSHELL_FOWMASK_SLOTS; ++i)
	{
		if(i == m_newest || i == m_sec->inUseSlot)
			continue;
		// claim the slot (invalid under its seqlock), then check the consumer did not name it meanwhile (it names a slot
		// first and checks the slot's record after; with full barriers on both sides one of the two sees the other)
		VPShellFowMaskSlot* m = VPSHELL_FOWMASK_SLOT(m_sec, i);
		InterlockedIncrement((volatile LONG*)&m->seq);
		MemoryBarrier();
		m->valid = 0;
		MemoryBarrier();
		InterlockedIncrement((volatile LONG*)&m->seq);
		MemoryBarrier();
		if(m_sec->inUseSlot == i)
			continue;
		Slot& s = m_slot[i];
		ctx->CopyResource(s.tex, mask);
		ctx->End(s.q);
		s.inflight = true;
		s.meta = in;
		LARGE_INTEGER q;
		QueryPerformanceCounter(&q);
		s.qpc = q.QuadPart;
		m_inflight = (int)i;
		m_polls = 0;
		m_pending = false;
		copies++;
		return;
	}
	m_pending = true;
	skipped++;
	InterlockedExchange((volatile LONG*)&m_sec->skippedTotal, (LONG)skipped);
}

bool FowMaskPublisher::consumerFailed(std::string& why) const
{
	if(!m_sec || m_sec->consumerState != VPFMC_FAILED)
		return false;
	char t[sizeof(m_sec->failText) + 1];
	memcpy(t, (const void*)m_sec->failText, sizeof(m_sec->failText));
	t[sizeof(m_sec->failText)] = 0;
	why = std::string("the EXE's model fog (shell_composite.dll) failed: ") + FailName(m_sec->failReason) + ": " + t;
	return true;
}

uint32_t FowMaskPublisher::consumerState() const { return m_sec ? m_sec->consumerState : (uint32_t)VPFMC_NONE; }

bool FowMaskPublisher::wanted() const { return m_sec && m_sec->wanted != 0; }

void FowMaskPublisher::stop(const char* why)
{
	if(!m_sec)
		return;
	const bool had = m_slot[0].tex != nullptr;
	dropSlots();
	InterlockedIncrement((volatile LONG*)&m_sec->seq);
	MemoryBarrier();
	m_sec->producerState = VPFMSK_STOPPED;
	MemoryBarrier();
	InterlockedIncrement((volatile LONG*)&m_sec->seq);
	m_pending = false;
	m_slotsFailed = false;
	if(had)
		note("stopped (%s): slot textures released; %lld published, %lld skipped, %lld faults", why, (long long)published,
		     (long long)skipped, (long long)faults);
}

void FowMaskPublisher::release(const char* why)
{
	stop(why);
	if(m_sec)
		UnmapViewOfFile(m_sec);
	m_sec = nullptr;
	if(m_secH)
		CloseHandle(m_secH);
	m_secH = nullptr;
	m_exePid = 0;
	m_lastConsumer = 0xFFFFFFFFu;
}

}  // namespace srvrender
