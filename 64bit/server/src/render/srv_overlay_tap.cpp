// srv_overlay_tap.cpp - server.exe's reader of the Terrain Overlay target tap (epic 0167, issue 3703; decision 1 route
// a, MC #696). Contract: srv_overlay_tap.h. Written 2026-11-14 for this repository (GPL-3.0-or-later). Producer:
// shell/composite/overlay_tap.cpp. Layout: VPShellOverlayTap / VPShellOverlaySection / VPShellOverlaySlot in
// shell/common/shell_shared.h. Design: research/05-graphics-composition/overlay-system.md sections 3-5, 7, 8.
//
// The slot hand-over (Dekker style, both sides with full barriers):
//   reader:   inUseSlot = s; barrier; read slot s (seq even, valid, generation = the block's) -> bind, ack; else go back
//   producer: slot s seq odd, valid = 0; barrier; read inUseSlot; if it is s -> restore s and do not write it
// so either the producer sees inUseSlot == s and keeps off it, or the reader sees s invalid and keeps its old pair.
#include "srv_overlay_tap.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <math.h>
#include <stdarg.h>
#include "vp_shared_tex.h"

namespace srvrender {

namespace {

const char* kStateNames[] = {"NONE", "ACTIVE", "FAILED", "OFF"};
const char* kFailNames[] = {"NONE", "PATTERN", "HANDLE", "DESC", "SHARED", "EXCEPTION", "VIEWS", "KILLED", "VERSION"};

const char* FailName(uint32_t r)
{
	return r < sizeof(kFailNames) / sizeof(kFailNames[0]) ? kFailNames[r] : "?";
}

std::string Fmt(const char* fmt, ...)
{
	char b[512];
	va_list a;
	va_start(a, fmt);
	_vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
	va_end(a);
	return b;
}

bool Rgba8Family(uint32_t f)
{
	return f >= DXGI_FORMAT_R8G8B8A8_TYPELESS && f <= DXGI_FORMAT_R8G8B8A8_SINT;   // 27..32
}

// A consistent copy of a slot (per-slot seqlock); false while the producer writes it.
bool SnapSlot(const VPShellOverlaySlot* src, VPShellOverlaySlot& out)
{
	for(int i = 0; i < 16; ++i)
	{
		const uint32_t s1 = src->seq;
		if(s1 & 1)
		{
			YieldProcessor();
			continue;
		}
		MemoryBarrier();
		memcpy(&out, (const void*)src, sizeof(out));
		MemoryBarrier();
		if(src->seq == s1)
		{
			out.seq = s1;
			return true;
		}
	}
	return false;
}

}  // namespace

struct OverlayTapReader::Impl
{
	ID3D11Device1* dev1 = nullptr;
	HANDLE secH = nullptr;
	const uint8_t* sec = nullptr;
	size_t secBytes = 0;
	uint32_t secPid = 0;
	struct Tex
	{
		ID3D11Texture2D* tex = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		uint32_t epoch = 0;
		uint32_t pid = 0;
	} tex[VPSHELL_OVERLAY_SLOTS];
	OverlayFrame bound;            // srv not referenced here: tex[bound.slot].srv owns it
	bool haveBound = false;
	uint32_t lastBlockState = 0xFFFFFFFFu;
	uint32_t lastBlockFail = 0xFFFFFFFFu;
	uint32_t lastGen = 0;
	int64_t lastGenPresent = 0;    // camera presentCount when the newest generation last moved
	uint64_t lastGenMapFrames = 0; // the block's copiesTotal + missedTotal then (map frames since the DLL started)
	uint64_t lastGenMs = 0;        // GetTickCount64() when the newest generation last moved
	bool staleEpisode = false;
	bool noMapGap = false;         // a gap of Presents without map frames was noted since the last new generation
	uint32_t refusedLogs = 0;
	std::string lastRefusal;

	void closeSection()
	{
		if(sec)
			UnmapViewOfFile(sec);
		if(secH)
			CloseHandle(secH);
		sec = nullptr;
		secH = nullptr;
		secBytes = 0;
		secPid = 0;
	}
	void dropTex(uint32_t i)
	{
		SafeRelease(tex[i].srv);
		SafeRelease(tex[i].tex);
		tex[i].epoch = 0;
		tex[i].pid = 0;
	}
	~Impl()
	{
		for(uint32_t i = 0; i < VPSHELL_OVERLAY_SLOTS; ++i)
			dropTex(i);
		closeSection();
		SafeRelease(dev1);
	}
};

OverlayTapReader::~OverlayTapReader()
{
	release();
}

void OverlayTapReader::release()
{
	if(m_hdr)
	{
		VPShellOverlayTap* t = VPSHELL_OVERLAYTAP(m_hdr);
		if(t->inUseSlot != 0xFFFFFFFFu)
			InterlockedExchange((volatile LONG*)&t->inUseSlot, (LONG)0xFFFFFFFFu);
	}
	delete m_impl;
	m_impl = nullptr;
}

bool OverlayTapReader::acquire(OverlayFrame& out)
{
	out = OverlayFrame{};
	if(!m_hdr || !m_dev)
	{
		m_state = State::Absent;
		return false;
	}
	if(m_state == State::Failed)
		return false;   // final: the producer's FAILED or a reader-side refusal (VPOVLF_VERSION); lastWhy() says why
	if(!m_impl)
		m_impl = new Impl();
	Impl& im = *m_impl;
	VPShellOverlayTap* live = VPSHELL_OVERLAYTAP(m_hdr);
	VPShellOverlayTap tb;
	if(!SnapBlock(live, tb))
	{
		// seq 0: no producer ever wrote the block (an old DLL, or the composite has not presented yet); odd: mid-write
		if(live->seq == 0)
			m_state = State::Absent;
		if(im.haveBound && m_state == State::Active)
		{
			out = im.bound;
			out.srv = im.tex[im.bound.slot].srv;
			return true;
		}
		return false;
	}
	if(tb.state != im.lastBlockState || tb.failReason != im.lastBlockFail)
	{
		RNote("overlay: tap block state %s (fail reason %s, pid %u, %u slots, newest slot %u generation %u, load seq %u, "
		      "%llu copies, %u missed, %u faults)",
		      tb.state < 4 ? kStateNames[tb.state] : "?", FailName(tb.failReason), tb.exePid, tb.slotCount, tb.newestSlot,
		      tb.generation, tb.loadSeq, (unsigned long long)tb.copiesTotal, live->missedTotal, live->faultsTotal);
		im.lastBlockState = tb.state;
		im.lastBlockFail = tb.failReason;
	}
	if(tb.state == VPOVL_FAILED)
	{
		m_failReason = tb.failReason;
		m_lastWhy = Fmt("the overlay tap could not install: VPOVL_FAILED %s (reason %u; shell_composite.log names the "
		                "detail)",
		                FailName(tb.failReason), tb.failReason);
		RNote("overlay: LOUD: %s", m_lastWhy.c_str());
		m_state = State::Failed;
		release();
		return false;
	}
	if(tb.state != VPOVL_ACTIVE)
	{
		const State s = tb.state == VPOVL_OFF ? State::Off : State::Waiting;
		if(s != m_state && im.haveBound)
			RNote("overlay: tap %s: the bound pair (generation %u) is dropped", s == State::Off ? "OFF" : "not active",
			      im.bound.generation);
		m_state = s;
		if(im.haveBound || live->inUseSlot != 0xFFFFFFFFu)
		{
			im.haveBound = false;
			InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)0xFFFFFFFFu);
		}
		return false;
	}

	// ---- ACTIVE: the section ----
	if(im.sec && im.secPid != tb.exePid)
	{
		RNote("overlay: the tap's EXE pid changed (%u -> %u): section and textures re-opened", im.secPid, tb.exePid);
		for(uint32_t i = 0; i < VPSHELL_OVERLAY_SLOTS; ++i)
			im.dropTex(i);
		im.closeSection();
		im.haveBound = false;
	}
	if(!im.sec)
	{
		wchar_t name[64];
		_snwprintf_s(name, _TRUNCATE, VPSHELL_OVERLAY_SECTION_FMT, tb.exePid);
		im.secH = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
		im.sec = im.secH ? (const uint8_t*)MapViewOfFile(im.secH, FILE_MAP_READ, 0, 0, 0) : nullptr;
		if(!im.sec)
		{
			const DWORD le = GetLastError();
			im.closeSection();
			refused++;
			const std::string why = Fmt("cannot open the overlay section '%ls' (error %lu) while the block is ACTIVE", name, le);
			if(why != im.lastRefusal && im.refusedLogs++ < 32)
				RNote("overlay: LOUD: %s", why.c_str());
			im.lastRefusal = why;
			m_lastWhy = why;
			m_state = State::Waiting;
			return false;
		}
		MEMORY_BASIC_INFORMATION mbi = {};
		im.secBytes = VirtualQuery(im.sec, &mbi, sizeof(mbi)) ? mbi.RegionSize : 0;
		im.secPid = tb.exePid;
		VPShellOverlaySection h = {};
		const size_t need = 64 + (size_t)VPSHELL_OVERLAY_SLOTS * sizeof(VPShellOverlaySlot);
		if(im.secBytes >= sizeof(h))
			memcpy(&h, im.sec, sizeof(h));
		std::string why;
		if(im.secBytes < need)
			why = Fmt("the overlay section '%ls' is %llu bytes, the layout needs %llu", name, (unsigned long long)im.secBytes,
			          (unsigned long long)need);
		else if(h.magic != VPSHELL_OVERLAY_MAGIC || h.version != VPSHELL_OVERLAY_SECTION_VERSION)
			why = Fmt("overlay section magic 0x%08X version %u, expected 0x%08X version %u", h.magic, h.version,
			          VPSHELL_OVERLAY_MAGIC, VPSHELL_OVERLAY_SECTION_VERSION);
		else if(h.slotBytes != sizeof(VPShellOverlaySlot) || h.slotCount != VPSHELL_OVERLAY_SLOTS ||
		        tb.slotCount != VPSHELL_OVERLAY_SLOTS)
			why = Fmt("overlay section slots %u x %u bytes (block %u slots), expected %u x %u", h.slotCount, h.slotBytes,
			          tb.slotCount, VPSHELL_OVERLAY_SLOTS, (unsigned)sizeof(VPShellOverlaySlot));
		else if(h.exePid != tb.exePid)
			why = Fmt("overlay section pid %u, the block's %u", h.exePid, tb.exePid);
		if(!why.empty())
		{
			im.closeSection();
			refused++;
			m_failReason = VPOVLF_VERSION;
			m_lastWhy = "the overlay tap's section is refused by the server: " + why;
			m_state = State::Failed;
			RNote("overlay: LOUD: %s (VPOVLF_VERSION)", m_lastWhy.c_str());
			release();
			return false;
		}
		RNote("overlay: section '%ls' open (%llu bytes, %u slots of %u bytes, EXE pid %u)", name,
		      (unsigned long long)im.secBytes, h.slotCount, h.slotBytes, h.exePid);
	}
	if(!im.dev1 && FAILED(m_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&im.dev1)))
	{
		im.dev1 = nullptr;
		m_failReason = VPOVLF_SHARED;
		m_lastWhy = "the server's device has no ID3D11Device1 (OpenSharedResource1)";
		m_state = State::Failed;
		RNote("overlay: LOUD: %s", m_lastWhy.c_str());
		release();
		return false;
	}
	m_state = State::Active;

	// ---- staleness: the newest generation against the target's MAP FRAMES (issue 0194, 2026-11-14) ----
	// A map frame is a Present that executed the scene-clear list; at each one after the install the DLL either issues a
	// copy (copiesTotal) or counts a missed one (missedTotal), so their sum counts the map frames. Frames without the
	// scene-clear list (AI leader screens, the load screen) draw no world and no overlay: the tap has nothing to copy and
	// the composite inserts no server frame, so a run of them is not a lag. Replaced: the threshold counted every
	// Present of the target, and each AI leader screen (0.56-0.97 s, 293-531 Presents) was one false episode (all 43 of
	// 3706 item 6's quiet ON arm; research/05-graphics-composition/overlay-system.md 11.1).
	const int64_t presents = VPSHELL_CAMERA(m_hdr)->presentCount;
	const uint64_t mapFrames = tb.copiesTotal + (uint64_t)live->missedTotal;
	if(tb.generation != im.lastGen)
	{
		if(im.staleEpisode)
		{
			const int64_t len = presents - im.lastGenPresent;
			if(len > staleLongestPresents)
				staleLongestPresents = len;
			if(stale <= (int64_t)VPOVL_READER_STALE_FULL_LINES)
				RNote("overlay: copies resumed (generation %u) after a stale episode of %lld Presents (%llu map frames)",
				      tb.generation, (long long)len, (unsigned long long)(mapFrames - im.lastGenMapFrames));
		}
		im.lastGen = tb.generation;
		im.lastGenPresent = presents;
		im.lastGenMapFrames = mapFrames;
		im.lastGenMs = GetTickCount64();
		im.staleEpisode = false;
		im.noMapGap = false;
	}
	else if(im.haveBound && !im.staleEpisode)
	{
		if(mapFrames < im.lastGenMapFrames)
			im.lastGenMapFrames = mapFrames;   // the counters of a new tap start (another EXE process): count from here
		const uint64_t ms = GetTickCount64() - im.lastGenMs;
		const int64_t mf = (int64_t)(mapFrames - im.lastGenMapFrames);
		if(mf > (int64_t)VPOVL_READER_STALE_MAP_FRAMES && ms >= VPOVL_READER_STALE_MS)
		{
			im.staleEpisode = true;
			stale++;
			// MC #891 (2026-11-14): a stale episode is a lag with the last consistent pair bound, not a fault (D132, R6):
			// the first VPOVL_READER_STALE_FULL_LINES episodes in full, then counted only; the run's count and longest
			// episode are in the scene's totals line ("overlay totals: ... stale episodes N longest P Presents")
			if(stale <= (int64_t)VPOVL_READER_STALE_FULL_LINES)
				RNote("overlay: stale (lag, counted, D132): no new copy for %lld map frames (%lld Presents of the target), "
				      "%llu ms (generation %u, copy of Present %llu; DLL counts %u missed, %u faults); the last pair stays bound",
				      (long long)mf, (long long)(presents - im.lastGenPresent), (unsigned long long)ms, tb.generation,
				      (unsigned long long)im.bound.presentCount, live->missedTotal, live->faultsTotal);
			if(stale == (int64_t)VPOVL_READER_STALE_FULL_LINES)
				RNote("overlay: further stale episodes are counted only (the run's count and longest episode are in the totals "
				      "line)");
		}
		else if(!im.noMapGap && presents - im.lastGenPresent > (int64_t)VPOVL_READER_STALE_MAP_FRAMES &&
		        ms >= VPOVL_READER_STALE_MS)
		{
			// the Presents went on without map frames: what the old rule counted as stale; not a lag (issue 0194)
			im.noMapGap = true;
			noMapGaps++;
			if(noMapGaps <= (int64_t)VPOVL_READER_STALE_FULL_LINES)
				RNote("overlay: %lld Presents of the target, %llu ms, with %lld map frames since the last copy (frames without "
				      "the scene-clear list, such as an AI leader screen): not a lag, not counted as stale (noted for the "
				      "first %u such gaps)",
				      (long long)(presents - im.lastGenPresent), (unsigned long long)ms, (long long)mf,
				      VPOVL_READER_STALE_FULL_LINES);
		}
	}

	auto keepOld = [&](bool ok) -> bool {
		if(im.haveBound)
		{
			out = im.bound;
			out.srv = im.tex[im.bound.slot].srv;
			return true;
		}
		return ok;
	};
	if(tb.generation == 0 || (im.haveBound && tb.generation == im.bound.generation))
		return keepOld(false);
	const uint32_t s = tb.newestSlot;
	auto refuse = [&](const std::string& why) -> bool {
		refused++;
		m_lastWhy = why;
		if(why != im.lastRefusal && im.refusedLogs++ < 32)
			RNote("overlay: LOUD: slot %u generation %u refused: %s%s", s, tb.generation, why.c_str(),
			      im.haveBound ? " (the last pair stays bound)" : "");
		im.lastRefusal = why;
		InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)(im.haveBound ? im.bound.slot : 0xFFFFFFFFu));
		return keepOld(false);
	};
	if(s >= VPSHELL_OVERLAY_SLOTS)
		return refuse(Fmt("newest slot %u of %u", s, VPSHELL_OVERLAY_SLOTS));

	// the hand-over: name the slot, then check it is still the published copy
	InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)s);
	MemoryBarrier();
	const VPShellOverlaySlot* ls = (const VPShellOverlaySlot*)(im.sec + 64 + (size_t)s * sizeof(VPShellOverlaySlot));
	VPShellOverlaySlot sl;
	if(!SnapSlot(ls, sl) || !sl.valid || sl.generation != tb.generation)
	{
		// the producer moved on (a newer copy, or it is writing this slot): not a fault, take the old pair this time
		InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)(im.haveBound ? im.bound.slot : 0xFFFFFFFFu));
		return keepOld(false);
	}
	if(sl.numViews == 0 || sl.numViews > VPSHELL_OVERLAY_MAX_VIEWS)
		return refuse(Fmt("numViews %u (1..%u)", sl.numViews, VPSHELL_OVERLAY_MAX_VIEWS));
	for(uint32_t v = 0; v < sl.numViews; ++v)
		for(int k = 0; k < 16; ++k)
			if(!isfinite(sl.overlayProj[v][k]))
				return refuse(Fmt("OverlayProj view %u element %d is not finite", v, k));
	if(sl.width != 2048 || sl.height != 2048 || sl.mips != 12 || !Rgba8Family(sl.dxgiFormat))
		return refuse(Fmt("slot describes %u x %u, %u mips, format %u (expected 2048 x 2048, 12 mips, R8G8B8A8)", sl.width,
		                  sl.height, sl.mips, sl.dxgiFormat));
	if(sl.presentCount == 0)
		return refuse("presentCount 0");

	Impl::Tex& tx = im.tex[s];
	if(!tx.tex || tx.epoch != sl.texEpoch || tx.pid != tb.exePid)
	{
		if(im.haveBound && im.bound.slot == s)
			im.haveBound = false;   // its SRV goes now
		im.dropTex(s);
		wchar_t name[80];
		_snwprintf_s(name, _TRUNCATE, VPSHELL_OVERLAY_TEXTURE_FMT, tb.exePid, s);
		ID3D11Texture2D* t = nullptr;
		VPSharedOpen how;
		HRESULT hr = VPOpenSharedTexture(im.dev1, tb.exePid, sl.ntHandle, name, DXGI_SHARED_RESOURCE_READ, &t, &how);
		if(FAILED(hr) || !t)
		{
			char why[400];
			return refuse(VPSharedFailText(why, sizeof(why), how, name, tb.exePid, sl.ntHandle));
		}
		VPShellOverlaySlot again;
		if(how.via == VPSHARED_HANDLE &&
		   (!SnapSlot(ls, again) || again.texEpoch != sl.texEpoch || again.ntHandle != sl.ntHandle))
		{
			// the DLL made the slot's texture again while we opened it: the value may name the new one; next frame
			t->Release();
			InterlockedExchange((volatile LONG*)&live->inUseSlot, (LONG)(im.haveBound ? im.bound.slot : 0xFFFFFFFFu));
			return keepOld(false);
		}
		D3D11_TEXTURE2D_DESC d;
		t->GetDesc(&d);
		if(d.Width != 2048 || d.Height != 2048 || d.MipLevels != 12 || d.ArraySize != 1 || d.SampleDesc.Count != 1 ||
		   !Rgba8Family(d.Format))
		{
			t->Release();
			return refuse(Fmt("shared texture '%ls' is %u x %u, %u mips, array %u, format %u (expected 2048 x 2048, 12 mips, "
			                  "R8G8B8A8)",
			                  name, d.Width, d.Height, d.MipLevels, d.ArraySize, (unsigned)d.Format));
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
		vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // the EXE's own SRV format (overlay-system.md 3)
		vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		vd.Texture2D.MostDetailedMip = 0;
		vd.Texture2D.MipLevels = 12;
		ID3D11ShaderResourceView* v = nullptr;
		hr = m_dev->CreateShaderResourceView(t, &vd, &v);
		if(FAILED(hr) || !v)
		{
			t->Release();
			return refuse(Fmt("CreateShaderResourceView on '%ls' failed 0x%08X", name, (unsigned)hr));
		}
		tx.tex = t;
		tx.srv = v;
		tx.epoch = sl.texEpoch;
		tx.pid = tb.exePid;
		RNote("overlay: slot %u texture '%ls' opened %s (epoch %u, format %u, 12 mips)", s, name, VPSharedViaText(how), sl.texEpoch,
		      (unsigned)d.Format);
	}
	// bound: ack
	InterlockedExchange((volatile LONG*)&live->ackServerPid, (LONG)GetCurrentProcessId());
	MemoryBarrier();
	InterlockedExchange((volatile LONG*)&live->ackGeneration, (LONG)sl.generation);
	OverlayFrame f;
	f.numViews = sl.numViews;
	memcpy(f.overlayProj, sl.overlayProj, sizeof(f.overlayProj));
	f.presentCount = sl.presentCount;
	f.generation = sl.generation;
	f.slot = s;
	if(!im.haveBound)
		RNote("overlay: first pair bound: slot %u generation %u, copy of Present %llu, %u views", s, sl.generation,
		      (unsigned long long)sl.presentCount, sl.numViews);
	im.bound = f;
	im.haveBound = true;
	accepted++;
	out = f;
	out.srv = tx.srv;
	return true;
}

}  // namespace srvrender
